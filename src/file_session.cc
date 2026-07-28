#include "readgen/file_session.hh"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <random>
#include <string>
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

class FileSession {
   public:
    FileSession(const FileSessionOptions& opts, FileSessionDone on_done, bool heap)
        : opts_(opts), on_done_(std::move(on_done)), heap_(heap) {
        result_.url = opts_.url;
        result_.vector = opts_.vector_chunks > 0;

        open_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnOpen(st, resp, hosts); });
        stat_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnStat(st, resp, hosts); });
        read_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnRead(st, resp, hosts); });
        close_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnClose(st, resp, hosts); });
    }

    // Submit Open. Returns false if submission failed (Complete already called).
    bool Start() {
        buffer_.resize(OpBytes());
        t_start_ = Clock::now();
        XrdCl::XRootDStatus st =
            file_.Open(opts_.url, XrdCl::OpenFlags::Read, XrdCl::Access::None, &open_handler_);
        if (!st.IsOK()) {
            result_.error = "open submission failed: " + st.ToString();
            result_.status_code = st.code;
            result_.err_code = st.errNo;
            Complete();
            return false;
        }
        return true;
    }

    FileSessionResult RunBlocking() {
        std::promise<void> done;
        auto fut = done.get_future();
        on_done_ = [&](FileSessionResult r) {
            result_ = std::move(r);
            done.set_value();
        };
        if (Start()) fut.get();
        return result_;
    }

   private:
    uint32_t OpBytes() const {
        return opts_.vector_chunks > 0 ? opts_.chunk_size * opts_.vector_chunks : opts_.chunk_size;
    }

    void Complete() {
        if (on_done_) {
            FileSessionDone cb = std::move(on_done_);
            FileSessionResult r = result_;
            // Self-delete when started via StartFileSession (heap). RunBlocking keeps stack.
            const bool heap = heap_;
            cb(std::move(r));
            if (heap) delete this;
        }
    }

    void Fail(const char* stage, XrdCl::XRootDStatus* st) {
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
        result_.throughput_mib_s =
            result_.read_s > 0 ? bytes_read_ / result_.read_s / (1024.0 * 1024.0) : 0.0;
        result_.op_lat_min_ms = ops_ > 0 ? lat_min_ms_ : 0.0;
        result_.op_lat_avg_ms = ops_ > 0 ? lat_sum_ms_ / ops_ : 0.0;
        result_.op_lat_max_ms = lat_max_ms_;

        Complete();
    }

    void OnOpen(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        t_open_ = Clock::now();
        if (hosts) {
            hops_ = hosts->size();
            delete hosts;
        }
        delete resp;
        if (!st->IsOK()) return Fail("open", st);
        delete st;

        file_.GetProperty("DataServer", data_server_);
        // Stat(false) after Open can complete synchronously from cached kXR_retstat
        // while still inside OpenHandler — nesting Read deadlocks JobManager.
        XrdCl::XRootDStatus s = file_.Stat(/*force=*/true, &stat_handler_);
        if (!s.IsOK()) {
            result_.error = "stat submission failed: " + s.ToString();
            result_.status_code = s.code;
            result_.err_code = s.errNo;
            Complete();
        }
    }

    void OnStat(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        delete hosts;
        if (!st->IsOK()) {
            delete resp;
            return Fail("stat", st);
        }
        delete st;

        XrdCl::StatInfo* info = nullptr;
        resp->Get(info);
        file_size_ = info ? info->GetSize() : 0;
        delete resp;

        uint64_t start = opts_.offset;
        uint64_t budget = file_size_;

        if (opts_.file_fraction > 0.0 && opts_.file_fraction < 1.0) {
            budget = static_cast<uint64_t>(static_cast<double>(file_size_) * opts_.file_fraction);
        }
        if (opts_.max_bytes > 0) budget = std::min(budget, opts_.max_bytes);

        if (opts_.random_offset && file_size_ > 0) {
            const uint64_t max_start = file_size_ > budget ? file_size_ - budget : 0;
            std::mt19937_64 rng(opts_.offset_seed);
            start = std::uniform_int_distribution<uint64_t>(0, max_start)(rng);
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
            return;
        }

        remaining_ = std::min(file_size_ - start, budget);
        next_offset_ = start;
        IssueNext();
    }

    void IssueNext() {
        if (remaining_ == 0) {
            t_last_byte_ = Clock::now();
            XrdCl::XRootDStatus s = file_.Close(&close_handler_);
            if (!s.IsOK()) {
                result_.error = "close submission failed: " + s.ToString();
                result_.status_code = s.code;
                result_.err_code = s.errNo;
                Complete();
            }
            return;
        }

        t_issue_ = Clock::now();
        XrdCl::XRootDStatus s;
        if (opts_.vector_chunks > 0) {
            XrdCl::ChunkList chunks;
            char* buf = buffer_.data();
            uint64_t off = next_offset_;
            uint64_t left = remaining_;
            for (uint16_t i = 0; i < opts_.vector_chunks && left > 0; ++i) {
                const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(opts_.chunk_size, left));
                chunks.emplace_back(off, len, buf);
                buf += len;
                off += len;
                left -= len;
            }
            requested_ = next_offset_ == off ? 0 : off - next_offset_;
            s = file_.VectorRead(chunks, nullptr, &read_handler_);
        } else {
            requested_ = static_cast<uint32_t>(std::min<uint64_t>(opts_.chunk_size, remaining_));
            s = file_.Read(next_offset_, static_cast<uint32_t>(requested_), buffer_.data(), &read_handler_);
        }
        if (!s.IsOK()) {
            result_.error = "read submission failed: " + s.ToString();
            result_.status_code = s.code;
            result_.err_code = s.errNo;
            Complete();
        }
    }

    void OnRead(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        const Clock::time_point now = Clock::now();
        delete hosts;
        if (!st->IsOK()) {
            delete resp;
            return Fail(opts_.vector_chunks > 0 ? "vector_read" : "read", st);
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
        next_offset_ += got;
        remaining_ = got < requested_ ? 0 : remaining_ - got;
        IssueNext();
    }

    void OnClose(XrdCl::XRootDStatus* st, XrdCl::AnyObject* resp, XrdCl::HostList* hosts) {
        t_end_ = Clock::now();
        delete hosts;
        delete resp;
        if (!st->IsOK()) return Fail("close", st);
        delete st;
        FinishOk();
    }

    const FileSessionOptions opts_;
    FileSessionDone on_done_;
    const bool heap_;
    FileSessionResult result_;
    XrdCl::File file_;
    std::vector<char> buffer_;

    FnHandler open_handler_, stat_handler_, read_handler_, close_handler_;

    std::string data_server_;
    uint64_t file_size_ = 0;
    uint64_t remaining_ = 0;
    uint64_t next_offset_ = 0;
    uint64_t requested_ = 0;
    uint64_t bytes_read_ = 0;
    uint64_t ops_ = 0;
    size_t hops_ = 0;

    double lat_sum_ms_ = 0.0;
    double lat_min_ms_ = std::numeric_limits<double>::max();
    double lat_max_ms_ = 0.0;

    Clock::time_point t_start_, t_open_, t_first_byte_, t_last_byte_, t_end_, t_issue_;
};

}  // namespace

FileSessionResult RunFileSession(const FileSessionOptions& opts) {
    FileSession session(opts, nullptr, /*heap=*/false);
    return session.RunBlocking();
}

void StartFileSession(const FileSessionOptions& opts, FileSessionDone on_done) {
    auto* session = new FileSession(opts, std::move(on_done), /*heap=*/true);
    (void)session->Start();  // On sync failure Complete() already deleted the session.
}

}  // namespace readgen
