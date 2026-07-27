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
#include <string>
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

// Adapter so each pipeline stage can be a member std::function on FileSession.
// XrdCl hands us ownership of status/response/hosts; the bound function frees
// them.
class FnHandler final : public XrdCl::ResponseHandler {
   public:
    using Fn = std::function<void(XrdCl::XRootDStatus*, XrdCl::AnyObject*, XrdCl::HostList*)>;

    void Set(Fn fn) { fn_ = std::move(fn); }

    // XrdCl may invoke either entry point depending on version/op. Override
    // both so we never fall through to the base no-op HandleResponse (which
    // hangs us forever on done_.get_future()).
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
    explicit FileSession(const FileSessionOptions& opts) : opts_(opts) {
        result_.url = opts_.url;
        result_.vector = opts_.vector_chunks > 0;

        open_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnOpen(st, resp, hosts); });
        stat_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnStat(st, resp, hosts); });
        read_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnRead(st, resp, hosts); });
        close_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnClose(st, resp, hosts); });
    }

    FileSessionResult Run() {
        buffer_.resize(OpBytes());

        t_start_ = Clock::now();
        XrdCl::XRootDStatus st =
            file_.Open(opts_.url, XrdCl::OpenFlags::Read, XrdCl::Access::None, &open_handler_);
        if (!st.IsOK()) {
            result_.error = "open submission failed: " + st.ToString();
            return result_;
        }

        done_.get_future().get();
        return result_;
    }

   private:
    uint32_t OpBytes() const {
        return opts_.vector_chunks > 0 ? opts_.chunk_size * opts_.vector_chunks : opts_.chunk_size;
    }

    void Fail(const char* stage, XrdCl::XRootDStatus* st) {
        result_.error = std::string(stage) + " failed: " + (st ? st->ToString() : "(no status)");
        delete st;
        done_.set_value();
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

        done_.set_value();
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
        // Stat(false) after Open can complete *synchronously* from the cached
        // kXR_retstat StatInfo while still inside OpenHandler. Nesting Read
        // from that re-entrant callback deadlocks XrdCl's JobManager. Force a
        // real round-trip so the Stat callback stays asynchronous.
        XrdCl::XRootDStatus s = file_.Stat(/*force=*/true, &stat_handler_);
        if (!s.IsOK()) {
            result_.error = "stat submission failed: " + s.ToString();
            done_.set_value();
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

        if (opts_.offset >= file_size_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "offset %" PRIu64 " is beyond file size %" PRIu64, opts_.offset,
                          file_size_);
            result_.error = buf;
            result_.file_size = file_size_;
            result_.data_server = data_server_;
            result_.open_hosts = hops_;
            done_.set_value();
            return;
        }

        remaining_ = file_size_ - opts_.offset;
        if (opts_.max_bytes > 0) remaining_ = std::min(remaining_, opts_.max_bytes);
        next_offset_ = opts_.offset;
        IssueNext();
    }

    void IssueNext() {
        if (remaining_ == 0) {
            t_last_byte_ = Clock::now();
            XrdCl::XRootDStatus s = file_.Close(&close_handler_);
            if (!s.IsOK()) {
                result_.error = "close submission failed: " + s.ToString();
                done_.set_value();
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
            done_.set_value();
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
        remaining_ = got < requested_ ? 0 : remaining_ - got;  // short read => EOF
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
    FileSessionResult result_;
    XrdCl::File file_;
    std::vector<char> buffer_;
    std::promise<void> done_;

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
    return FileSession(opts).Run();
}

}  // namespace readgen
