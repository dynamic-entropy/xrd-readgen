#include "readgen/file_session.hh"
#include "readgen/random_io.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace readgen {
namespace {

using Clock = std::chrono::steady_clock;

double MsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double SecsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

class FnHandler final : public XrdCl::ResponseHandler {
   public:
    using Fn = std::function<void(XrdCl::XRootDStatus*, XrdCl::AnyObject*, XrdCl::HostList*)>;

    void Set(Fn fn) { fn_ = std::move(fn); }

    void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                                 XrdCl::HostList* hosts) override {
        fn_(status, response, hosts);
    }

    void HandleResponse(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response) override {
        fn_(status, response, nullptr);
    }

   private:
    Fn fn_;
};

class FileSession;

// One shared deadline thread for all sessions (avoids one detached 50 ms poller
// per in-flight session). Entries are weak; completed sessions drop out.
// Must be ShutDown() before process exit — a detached forever-loop races static
// destruction and SEGV's on IsCompleted (see exit-path cores).
class SessionDeadlineWatchdog {
   public:
    static SessionDeadlineWatchdog& Instance() {
        // Heap-allocated so exit-time static dtors do not tear us down under the
        // worker thread if ShutDown was skipped.
        static SessionDeadlineWatchdog* w = new SessionDeadlineWatchdog();
        return *w;
    }

    void Arm(const std::shared_ptr<FileSession>& session, double timeout_s) {
        if (timeout_s <= 0.0 || !session) return;
        const auto deadline =
            Clock::now() +
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_s));
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            entries_.push_back(Entry{deadline, session});
            EnsureThreadLocked();
        }
        cv_.notify_one();
    }

    void ShutDown() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            stop_ = true;
            entries_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(mu_);
        started_ = false;
        stop_ = false;  // allow a later run in the same process
    }

   private:
    struct Entry {
        Clock::time_point deadline;
        std::weak_ptr<FileSession> session;
    };

    SessionDeadlineWatchdog() = default;
    SessionDeadlineWatchdog(const SessionDeadlineWatchdog&) = delete;
    SessionDeadlineWatchdog& operator=(const SessionDeadlineWatchdog&) = delete;

    void EnsureThreadLocked() {
        if (started_) return;
        started_ = true;
        stop_ = false;
        thread_ = std::thread([this] { ThreadMain(); });
    }

    void ThreadMain();

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
    bool started_ = false;
    bool stop_ = false;
    std::thread thread_;
};

class FileSession : public std::enable_shared_from_this<FileSession> {
   public:
    FileSession(const FileSessionOptions& opts, FileSessionDone on_done)
        : opts_(opts), on_done_(std::move(on_done)) {
        result_.url = opts_.url;
        result_.vector = opts_.vector_chunks > 0;

        open_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnOpen(st, resp, hosts); });
        stat_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnStat(st, resp, hosts); });
        read_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnRead(st, resp, hosts); });
        close_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnClose(st, resp, hosts); });
    }

    // Submit Open. Keeps a self-reference until the session is completed AND no
    // XrdCl callback is outstanding (see ReleaseIfIdle).
    bool Start() {
        std::unique_lock<std::mutex> lock(smu_);
        self_keep_ = shared_from_this();
        buffer_.resize(OpBytes());
        t_start_ = Clock::now();
        if (opts_.wall_timeout_s > 0.0) {
            SessionDeadlineWatchdog::Instance().Arm(shared_from_this(), opts_.wall_timeout_s);
        }
        ++pending_cbs_;
        XrdCl::XRootDStatus st =
            file_.Open(opts_.url, XrdCl::OpenFlags::Read, XrdCl::Access::None, &open_handler_);
        if (!st.IsOK()) {
            --pending_cbs_;
            result_.error = "open submission failed: " + st.ToString();
            result_.status_code = st.code;
            result_.err_code = st.errNo;
            Complete();
            ReleaseIfIdle(lock);
            return false;
        }
        return true;
    }

    bool IsCompleted() const { return completed_.load(std::memory_order_acquire); }

    // Invoked by the shared watchdog. Never sync-Close from the watchdog thread —
    // that races XrdCl handlers and corrupts the heap under load. Ask for an
    // async Close instead; OnClose completes the session. XrdCl rejects Close
    // while another op is in flight (errInvalidOp), in which case SubmitClose
    // rolls back and the in-flight op's handler retries the close on completion
    // (IssueNext observes timed_out_).
    void RequestWallTimeout() {
        std::unique_lock<std::mutex> lock(smu_);
        if (completed_.load(std::memory_order_acquire)) return;
        if (close_requested_) return;  // normal Close in flight — not a timeout
        timed_out_ = true;
        SubmitClose();
        ReleaseIfIdle(lock);
    }

   private:
    // All private methods below require smu_ to be held. Handlers must call
    // ReleaseIfIdle as their very last action: it may destroy *this.
    uint32_t OpBytes() const {
        return opts_.vector_chunks > 0 ? opts_.chunk_size * opts_.vector_chunks : opts_.chunk_size;
    }

    void CapturePartialCounters() {
        result_.bytes_read = bytes_read_;
        result_.ops = ops_;
        result_.data_server = data_server_;
        result_.file_size = file_size_;
        result_.open_hosts = hops_;
        // Preserve open/TTFB when available so failed probes still report latency.
        if (t_open_.time_since_epoch().count() != 0) {
            result_.open_ms = MsBetween(t_start_, t_open_);
            if (ops_ > 0 && t_first_byte_.time_since_epoch().count() != 0) {
                result_.ttfb_ms = MsBetween(t_open_, t_first_byte_);
            }
        }
        result_.total_s = SecsBetween(t_start_, Clock::now());
    }

    // Marks the session done and fires on_done. Does NOT drop self_keep_: the
    // object must stay alive until every submitted XrdCl handler has run, or a
    // late callback dereferences a destroyed handler (observed SEGV in
    // HandleResponseWithHosts after wall timeouts).
    void Complete() {
        bool expected = false;
        if (!completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (on_done_) {
            FileSessionDone cb = std::move(on_done_);
            cb(result_);
        }
    }

    // Drops the self-reference once the session is completed and no XrdCl
    // callback is outstanding. May destroy *this on scope exit, so callers must
    // not touch members afterwards. Always unlocks `lock`.
    void ReleaseIfIdle(std::unique_lock<std::mutex>& lock) {
        std::shared_ptr<FileSession> keep;
        if (pending_cbs_ == 0 && completed_.load(std::memory_order_acquire)) {
            keep = std::move(self_keep_);
        }
        lock.unlock();
        // `keep` (possibly the last reference) is destroyed here, after unlock.
    }

    // Submits the async Close exactly once. If XrdCl rejects it because another
    // op is in flight, rolls back so that op's handler retries via IssueNext.
    // Completes the session on a definitive close failure.
    void SubmitClose() {
        if (close_requested_) return;
        close_requested_ = true;
        ++pending_cbs_;
        XrdCl::XRootDStatus s = file_.Close(&close_handler_);
        if (s.IsOK()) return;
        --pending_cbs_;
        if (pending_cbs_ > 0) {
            close_requested_ = false;
            return;
        }
        CapturePartialCounters();
        if (timed_out_) {
            result_.timed_out = true;
            result_.error = "session wall timeout";
            result_.status_code = s.code;
            result_.err_code = 110;  // ETIMEDOUT
        } else {
            result_.error = "close submission failed: " + s.ToString();
            result_.status_code = s.code;
            result_.err_code = s.errNo;
        }
        Complete();
    }

    void Fail(const char* stage, XrdCl::XRootDStatus* st) {
        CapturePartialCounters();
        result_.error = std::string(stage) + " failed: " + (st ? st->ToString() : "(no status)");
        if (st) {
            result_.status_code = st->code;
            result_.err_code = st->errNo;
        }
        delete st;
        Complete();
    }

    void FinishOk() {
        result_.ok = true;
        result_.data_server = data_server_;
        result_.file_size = file_size_;
        result_.open_hosts = hops_;
        result_.bytes_read = bytes_read_;
        result_.ops = ops_;

        result_.open_ms = MsBetween(t_start_, t_open_);
        result_.ttfb_ms = ops_ > 0 ? MsBetween(t_open_, t_first_byte_) : 0.0;
        result_.read_s = SecsBetween(t_open_, t_last_byte_);
        result_.close_ms = MsBetween(t_last_byte_, t_end_);
        result_.total_s = SecsBetween(t_start_, t_end_);
        result_.throughput_Mbps =
            result_.read_s > 0 ? (bytes_read_ / result_.read_s) * 8.0 / 1e6 : 0.0;
        result_.op_lat_min_ms = ops_ > 0 ? lat_min_ms_ : 0.0;
        result_.op_lat_avg_ms = ops_ > 0 ? lat_sum_ms_ / ops_ : 0.0;
        result_.op_lat_max_ms = lat_max_ms_;

        Complete();
    }

    void OnOpen(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        std::unique_lock<std::mutex> lock(smu_);
        --pending_cbs_;
        if (completed_.load(std::memory_order_acquire)) {
            delete hosts;
            delete resp;
            delete st;
            ReleaseIfIdle(lock);
            return;
        }
        t_open_ = Clock::now();
        if (hosts) {
            hops_ = hosts->size();
            delete hosts;
        }
        delete resp;
        if (!st->IsOK()) {
            Fail("open", st);
            ReleaseIfIdle(lock);
            return;
        }
        delete st;

        file_.GetProperty("DataServer", data_server_);
        if (timed_out_) {
            SubmitClose();
            ReleaseIfIdle(lock);
            return;
        }
        ++pending_cbs_;
        XrdCl::XRootDStatus s = file_.Stat(/*force=*/true, &stat_handler_);
        if (!s.IsOK()) {
            --pending_cbs_;
            result_.error = "stat submission failed: " + s.ToString();
            result_.status_code = s.code;
            result_.err_code = s.errNo;
            Complete();
        }
        ReleaseIfIdle(lock);
    }

    void OnStat(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        std::unique_lock<std::mutex> lock(smu_);
        --pending_cbs_;
        if (completed_.load(std::memory_order_acquire)) {
            delete hosts;
            delete resp;
            delete st;
            ReleaseIfIdle(lock);
            return;
        }
        delete hosts;
        if (!st->IsOK()) {
            delete resp;
            Fail("stat", st);
            ReleaseIfIdle(lock);
            return;
        }
        delete st;

        XrdCl::StatInfo* info = nullptr;
        resp->Get(info);
        file_size_ = info ? info->GetSize() : 0;
        delete resp;

        uint64_t start = opts_.offset;
        uint64_t budget = file_size_;

        if (opts_.max_bytes > 0) budget = std::min(budget, opts_.max_bytes);

        if (opts_.random_offset) {
            // Per-op random I/O: budget is the session byte cap; offsets are
            // sampled independently in IssueNext (not a random-start walk).
            rng_ = std::mt19937_64(opts_.offset_seed);
            remaining_ = budget;
            next_offset_ = 0;
            IssueNext();
            ReleaseIfIdle(lock);
            return;
        }

        if (start >= file_size_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "offset %" PRIu64 " is beyond file size %" PRIu64, start,
                          file_size_);
            result_.error = buf;
            result_.file_size = file_size_;
            result_.data_server = data_server_;
            result_.open_hosts = hops_;
            Complete();
            ReleaseIfIdle(lock);
            return;
        }

        remaining_ = std::min(file_size_ - start, budget);
        next_offset_ = start;
        IssueNext();
        ReleaseIfIdle(lock);
    }

    void IssueNext() {
        if (completed_.load(std::memory_order_acquire)) return;
        if (timed_out_) remaining_ = 0;
        if (remaining_ == 0) {
            t_last_byte_ = Clock::now();
            SubmitClose();
            return;
        }

        t_issue_ = Clock::now();
        XrdCl::XRootDStatus s;
        ++pending_cbs_;
        if (opts_.vector_chunks > 0) {
            XrdCl::ChunkList chunks;
            char* buf = buffer_.data();
            uint64_t left = remaining_;
            uint64_t requested = 0;
            if (opts_.random_offset) {
                for (uint16_t i = 0; i < opts_.vector_chunks && left > 0; ++i) {
                    const uint32_t len =
                        static_cast<uint32_t>(std::min<uint64_t>(opts_.chunk_size, left));
                    const uint64_t off = SampleRandomReadOffset(rng_, file_size_, len);
                    chunks.emplace_back(off, len, buf);
                    buf += len;
                    left -= len;
                    requested += len;
                }
            } else {
                uint64_t off = next_offset_;
                for (uint16_t i = 0; i < opts_.vector_chunks && left > 0; ++i) {
                    const uint32_t len =
                        static_cast<uint32_t>(std::min<uint64_t>(opts_.chunk_size, left));
                    chunks.emplace_back(off, len, buf);
                    buf += len;
                    off += len;
                    left -= len;
                }
                requested = next_offset_ == off ? 0 : off - next_offset_;
            }
            requested_ = requested;
            s = file_.VectorRead(chunks, nullptr, &read_handler_);
        } else {
            requested_ = static_cast<uint32_t>(std::min<uint64_t>(opts_.chunk_size, remaining_));
            if (opts_.random_offset) {
                next_offset_ = SampleRandomReadOffset(rng_, file_size_,
                                                       static_cast<uint32_t>(requested_));
            }
            s = file_.Read(next_offset_, static_cast<uint32_t>(requested_), buffer_.data(),
                           &read_handler_);
        }
        if (!s.IsOK()) {
            --pending_cbs_;
            CapturePartialCounters();
            result_.error = "read submission failed: " + s.ToString();
            result_.status_code = s.code;
            result_.err_code = s.errNo;
            Complete();
        }
    }

    void OnRead(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        std::unique_lock<std::mutex> lock(smu_);
        --pending_cbs_;
        if (completed_.load(std::memory_order_acquire)) {
            delete hosts;
            delete resp;
            delete st;
            ReleaseIfIdle(lock);
            return;
        }
        const Clock::time_point now = Clock::now();
        delete hosts;
        if (!st->IsOK()) {
            delete resp;
            Fail(opts_.vector_chunks > 0 ? "vector_read" : "read", st);
            ReleaseIfIdle(lock);
            return;
        }
        delete st;

        if (ops_ == 0) t_first_byte_ = now;
        ++ops_;

        const double lat = MsBetween(t_issue_, now);
        lat_sum_ms_ += lat;
        lat_min_ms_ = std::min(lat_min_ms_, lat);
        lat_max_ms_ = std::max(lat_max_ms_, lat);

        uint64_t got = 0;
        if (opts_.vector_chunks > 0) {
            XrdCl::VectorReadInfo* vi = nullptr;
            resp->Get(vi);
            got = vi ? vi->GetSize() : requested_;
        } else {
            XrdCl::ChunkInfo* ci = nullptr;
            resp->Get(ci);
            got = ci ? ci->GetLength() : 0;
        }
        delete resp;

        bytes_read_ += got;
        if (!opts_.random_offset) {
            next_offset_ += got;
        }
        remaining_ = got < requested_ ? 0 : remaining_ - got;
        IssueNext();
        ReleaseIfIdle(lock);
    }

    void OnClose(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        std::unique_lock<std::mutex> lock(smu_);
        --pending_cbs_;
        if (completed_.load(std::memory_order_acquire)) {
            delete hosts;
            delete resp;
            delete st;
            ReleaseIfIdle(lock);
            return;
        }
        t_end_ = Clock::now();
        delete hosts;
        delete resp;
        if (timed_out_) {
            CapturePartialCounters();
            result_.timed_out = true;
            result_.error = "session wall timeout";
            result_.status_code = st ? st->code : 0;
            result_.err_code = 110;
            delete st;
            Complete();
            ReleaseIfIdle(lock);
            return;
        }
        if (!st->IsOK()) {
            Fail("close", st);
            ReleaseIfIdle(lock);
            return;
        }
        delete st;
        FinishOk();
        ReleaseIfIdle(lock);
    }

    const FileSessionOptions opts_;
    FileSessionDone on_done_;
    FileSessionResult result_;
    XrdCl::File file_;
    std::vector<char> buffer_;
    std::shared_ptr<FileSession> self_keep_;

    FnHandler open_handler_, stat_handler_, read_handler_, close_handler_;

    std::string data_server_;
    uint64_t file_size_ = 0;
    uint64_t remaining_ = 0;
    uint64_t next_offset_ = 0;
    uint64_t requested_ = 0;
    uint64_t bytes_read_ = 0;
    uint64_t ops_ = 0;
    size_t hops_ = 0;
    std::mt19937_64 rng_{};

    double lat_sum_ms_ = 0.0;
    double lat_min_ms_ = std::numeric_limits<double>::max();
    double lat_max_ms_ = 0.0;

    Clock::time_point t_start_, t_open_, t_first_byte_, t_last_byte_, t_end_, t_issue_;

    // Serializes all session state transitions between XrdCl handler threads
    // and the deadline watchdog. XrdCl invokes response handlers without
    // holding file-state locks, so submitting ops under smu_ is safe.
    std::mutex smu_;
    // Outstanding XrdCl callbacks still expected (guarded by smu_). The session
    // may only be released once this drops to zero.
    int pending_cbs_ = 0;
    bool timed_out_ = false;        // guarded by smu_
    bool close_requested_ = false;  // guarded by smu_
    std::atomic<bool> completed_{false};  // atomic: watchdog reads it lock-free
};

void SessionDeadlineWatchdog::ThreadMain() {
    constexpr auto kMaxSleep = std::chrono::seconds(1);
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        const auto now = Clock::now();
        Clock::time_point next_deadline = now + kMaxSleep;
        std::vector<std::shared_ptr<FileSession>> expired;

        // Prune dead entries and collect sessions past their deadline.
        size_t out = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            auto self = entries_[i].session.lock();
            if (!self || self->IsCompleted()) {
                continue;
            }
            if (entries_[i].deadline <= now) {
                expired.push_back(std::move(self));
                continue;
            }
            next_deadline = std::min(next_deadline, entries_[i].deadline);
            if (out != i) entries_[out] = std::move(entries_[i]);
            ++out;
        }
        entries_.resize(out);

        lock.unlock();
        for (auto& s : expired) {
            s->RequestWallTimeout();
        }
        lock.lock();
        if (stop_) break;

        auto wait_for = next_deadline - Clock::now();
        if (wait_for < std::chrono::milliseconds(1)) wait_for = std::chrono::milliseconds(1);
        if (wait_for > kMaxSleep) wait_for = kMaxSleep;
        cv_.wait_for(lock, wait_for, [this] { return stop_; });
    }
}

}  // namespace

FileSessionResult RunFileSession(const FileSessionOptions& opts) {
    std::promise<FileSessionResult> done;
    auto fut = done.get_future();
    auto session = std::make_shared<FileSession>(opts, [&](FileSessionResult r) {
        done.set_value(std::move(r));
    });
    // On sync submission failure Start already ran Complete, so fut is ready.
    (void)session->Start();
    return fut.get();
}

void StartFileSession(const FileSessionOptions& opts, FileSessionDone on_done) {
    auto session = std::make_shared<FileSession>(opts, std::move(on_done));
    (void)session->Start();
}

void ShutdownSessionWatchdog() { SessionDeadlineWatchdog::Instance().ShutDown(); }

}  // namespace readgen
