#include "readgen/read_command.hh"

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

double SecsBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// Adapter so each pipeline stage can be a member std::function on TimedReader.
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

class TimedReader {
   public:
    explicit TimedReader(const ReadOptions& opts) : opts_(opts) {
        open_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnOpen(st, resp, hosts); });
        stat_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnStat(st, resp, hosts); });
        read_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnRead(st, resp, hosts); });
        close_handler_.Set([this](auto* st, auto* resp, auto* hosts) { OnClose(st, resp, hosts); });
    }

    int Run() {
        const uint32_t op_bytes = OpBytes();
        buffer_.resize(op_bytes);

        t_start_ = Clock::now();
        XrdCl::XRootDStatus st = file_.Open(opts_.url, XrdCl::OpenFlags::Read, XrdCl::Access::None, &open_handler_);
        if (!st.IsOK()) {
            std::fprintf(stderr, "error: open submission failed: %s\n", st.ToString().c_str());
            return 1;
        }

        int rc = done_.get_future().get();
        if (rc == 0) Report();
        return rc;
    }

   private:
    uint32_t OpBytes() const {
        return opts_.vector_chunks > 0 ? opts_.chunk_size * opts_.vector_chunks : opts_.chunk_size;
    }

    void Fail(const char* stage, XrdCl::XRootDStatus* st) {
        std::fprintf(stderr, "error: %s failed: %s\n", stage, st ? st->ToString().c_str() : "(no status)");
        delete st;
        done_.set_value(1);
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
            std::fprintf(stderr, "error: stat submission failed: %s\n", s.ToString().c_str());
            done_.set_value(1);
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
            std::fprintf(stderr, "error: offset %" PRIu64 " is beyond file size %" PRIu64 "\n", opts_.offset,
                         file_size_);
            done_.set_value(1);
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
                std::fprintf(stderr, "error: close submission failed: %s\n", s.ToString().c_str());
                done_.set_value(1);
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
            std::fprintf(stderr, "error: read submission failed: %s\n", s.ToString().c_str());
            done_.set_value(1);
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
        done_.set_value(0);
    }

    void Report() const {
        const double open_ms = MsBetween(t_start_, t_open_);
        const double ttfb_ms = MsBetween(t_open_, t_first_byte_);
        const double read_s = SecsBetween(t_open_, t_last_byte_);
        const double close_ms = MsBetween(t_last_byte_, t_end_);
        const double total_s = SecsBetween(t_start_, t_end_);
        const double mibps = read_s > 0 ? bytes_read_ / read_s / (1024.0 * 1024.0) : 0.0;
        const double lat_avg = ops_ > 0 ? lat_sum_ms_ / ops_ : 0.0;

        if (opts_.json) {
            std::printf("{\"url\":\"%s\",\"data_server\":\"%s\",\"file_size\":%" PRIu64
                        ",\"open_hosts\":%zu,\"open_ms\":%.3f,\"ttfb_ms\":%.3f"
                        ",\"bytes_read\":%" PRIu64 ",\"ops\":%" PRIu64
                        ",\"op\":\"%s\",\"read_s\":%.3f,\"throughput_mib_s\":%.2f"
                        ",\"op_latency_ms\":{\"min\":%.3f,\"avg\":%.3f,\"max\":%.3f}"
                        ",\"close_ms\":%.3f,\"total_s\":%.3f}\n",
                        opts_.url.c_str(), data_server_.c_str(), file_size_, hops_, open_ms, ttfb_ms, bytes_read_, ops_,
                        opts_.vector_chunks > 0 ? "vector_read" : "read", read_s, mibps, lat_min_ms_, lat_avg,
                        lat_max_ms_, close_ms, total_s);
            return;
        }

        std::printf("URL:          %s\n", opts_.url.c_str());
        std::printf("Data server:  %s\n", data_server_.c_str());
        std::printf("File size:    %" PRIu64 " bytes\n", file_size_);
        std::printf("Open:         %.3f ms (%zu host%s in open chain)\n", open_ms, hops_, hops_ == 1 ? "" : "s");
        std::printf("TTFB:         %.3f ms\n", ttfb_ms);
        std::printf("Read:         %" PRIu64 " bytes in %" PRIu64 " %s op%s (%.3f s)\n", bytes_read_, ops_,
                    opts_.vector_chunks > 0 ? "vector_read" : "read", ops_ == 1 ? "" : "s", read_s);
        std::printf("Op latency:   min %.3f / avg %.3f / max %.3f ms\n", lat_min_ms_, lat_avg, lat_max_ms_);
        std::printf("Throughput:   %.2f MiB/s\n", mibps);
        std::printf("Close:        %.3f ms\n", close_ms);
        std::printf("Total:        %.3f s\n", total_s);
    }

    const ReadOptions opts_;
    XrdCl::File file_;
    std::vector<char> buffer_;
    std::promise<int> done_;

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

int RunReadCommand(const ReadOptions& opts) {
    TimedReader reader(opts);
    return reader.Run();
}

}  // namespace readgen
