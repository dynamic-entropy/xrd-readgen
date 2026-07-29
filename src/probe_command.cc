#include "readgen/probe_command.hh"

#include "readgen/auth_check.hh"
#include "readgen/error_classifier.hh"
#include "readgen/inflight.hh"
#include "readgen/run_config.hh"
#include "readgen/workload_spec.hh"
#include "readgen/xrdcl_env.hh"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace readgen {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct ProbeItemResult {
    std::string target;
    std::string url;
    bool ok = false;
    bool timed_out = false;
    std::string error_class = "none";
    std::string error;
    double open_ms = 0.0;
    double ttfb_ms = 0.0;
    size_t open_hosts = 0;
    std::string data_server;
};

json ItemToJson(const ProbeItemResult& r) {
    return json{{"target", r.target},
                {"url", r.url},
                {"ok", r.ok},
                {"timed_out", r.timed_out},
                {"error_class", r.error_class},
                {"error", r.error},
                {"open_ms", r.open_ms},
                {"ttfb_ms", r.ttfb_ms},
                {"open_hosts", r.open_hosts},
                {"data_server", r.data_server}};
}

void PrintHuman(const ProbeItemResult& r) {
    if (r.ok) {
        std::printf("OK  %-16s open=%7.1f ms ttfb=%7.1f ms hosts=%zu ds=%s\n  %s\n",
                    r.target.c_str(), r.open_ms, r.ttfb_ms, r.open_hosts,
                    r.data_server.empty() ? "-" : r.data_server.c_str(), r.url.c_str());
    } else {
        std::printf("FAIL %-16s class=%-14s open=%7.1f ms ttfb=%7.1f ms hosts=%zu ds=%s\n  %s\n  %s\n",
                    r.target.c_str(), r.error_class.c_str(), r.open_ms, r.ttfb_ms, r.open_hosts,
                    r.data_server.empty() ? "-" : r.data_server.c_str(), r.url.c_str(),
                    r.error.c_str());
    }
}

ProbeItemResult FromSession(const std::string& target, const std::string& url,
                            const FileSessionResult& r) {
    ProbeItemResult out;
    out.target = target;
    out.url = url;
    out.ok = r.ok;
    out.timed_out = r.timed_out;
    out.open_ms = r.open_ms;
    out.ttfb_ms = r.ttfb_ms;
    out.open_hosts = r.open_hosts;
    out.data_server = r.data_server;
    if (!r.ok) {
        const ErrorClass cls =
            r.timed_out ? ErrorClass::Timeout
                        : ClassifyXRootDError(r.status_code, r.err_code, r.error);
        out.error_class = ErrorClassName(cls);
        out.error = r.error;
    }
    return out;
}

}  // namespace

int RunProbeCommand(const ProbeOptions& opts) {
    const auto validated = ValidateWorkloadFile(opts.workload_path);
    if (!validated.ok) {
        for (const auto& issue : validated.issues) {
            if (issue.field.empty()) {
                std::fprintf(stderr, "error: %s\n", issue.message.c_str());
            } else {
                std::fprintf(stderr, "%s: %s\n", issue.field.c_str(), issue.message.c_str());
            }
        }
        return 2;
    }

    const WorkloadSpec& wl = validated.resolved;
    AuthCheckResult auth;
    auth.ok = true;
    if (!opts.skip_auth_check) {
        if (wl.auth.mode != "x509") {
            std::fprintf(stderr, "auth.mode: unsupported auth mode for probe\n");
            return 2;
        }
        auth = CheckX509Credentials(wl.duration_s);
        if (!auth.ok) {
            for (const auto& issue : auth.issues) {
                std::fprintf(stderr, "%s: %s\n", issue.field.c_str(), issue.message.c_str());
            }
            return 2;
        }
    }

    std::vector<const TargetSpec*> targets;
    for (const auto& t : wl.targets) {
        if (!opts.target.empty() && t.name != opts.target) continue;
        targets.push_back(&t);
    }
    if (targets.empty()) {
        if (!opts.target.empty()) {
            std::fprintf(stderr, "target: unknown target name '%s'\n", opts.target.c_str());
        } else {
            std::fprintf(stderr, "targets: no targets to probe\n");
        }
        return 2;
    }

    struct WorkItem {
        std::string target;
        std::string url;
    };
    std::vector<WorkItem> work;
    for (const TargetSpec* t : targets) {
        size_t n = t->files.size();
        if (opts.limit > 0 && static_cast<size_t>(opts.limit) < n) n = opts.limit;
        for (size_t i = 0; i < n; ++i) {
            work.push_back(WorkItem{t->name, JoinUrl(t->endpoint, t->files[i])});
        }
    }
    if (work.empty()) {
        std::fprintf(stderr, "probe: no files selected\n");
        return 2;
    }

    ApplyXrdClTimeouts(wl.client_tuning.connection_window_s, wl.client_tuning.connection_retry,
                       wl.client_tuning.request_timeout_s, wl.client_tuning.session_timeout_s,
                       /*log=*/true);

    const uint32_t concurrency = opts.concurrency == 0 ? 1 : opts.concurrency;
    InFlightSemaphore inflight(concurrency);
    ProbeSessionRunner runner = opts.session_runner;
    if (!runner) {
        runner = [](const FileSessionOptions& o, FileSessionDone done) {
            StartFileSession(o, std::move(done));
        };
    }

    std::mutex mu;
    std::condition_variable cv;
    size_t completed = 0;
    std::vector<ProbeItemResult> results;
    results.reserve(work.size());

    const auto deadline = Clock::now() + std::chrono::hours(24);
    size_t admitted = 0;
    for (const auto& item : work) {
        if (!inflight.AcquireUntil(deadline)) break;
        ++admitted;

        FileSessionOptions so;
        so.url = item.url;
        so.chunk_size = 1;
        so.max_bytes = 1;
        so.vector_chunks = 0;
        so.file_fraction = 1.0;
        so.wall_timeout_s = wl.client_tuning.session_timeout_s;

        const std::string target_name = item.target;
        const std::string url = item.url;
        runner(so, [&, target_name, url](FileSessionResult r) {
            ProbeItemResult out = FromSession(target_name, url, r);
            {
                std::lock_guard<std::mutex> lock(mu);
                results.push_back(std::move(out));
                ++completed;
            }
            inflight.Release();
            cv.notify_all();
        });
    }

    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return completed >= admitted; });
    }
    ShutdownSessionWatchdog();

    std::map<std::string, uint64_t> by_class;
    uint64_t ok_n = 0;
    uint64_t fail_n = 0;
    for (const auto& r : results) {
        if (r.ok) {
            ++ok_n;
        } else {
            ++fail_n;
            ++by_class[r.error_class];
        }
    }

    if (opts.json) {
        json j;
        j["workload_hash"] = validated.workload_hash;
        j["run_id"] = wl.run_id;
        j["auth_skipped"] = opts.skip_auth_check;
        if (!opts.skip_auth_check) {
            j["auth_remaining_s"] = auth.remaining_s;
            j["auth_proxy_path"] = auth.proxy_path;
        }
        j["probed"] = results.size();
        j["ok"] = ok_n;
        j["failed"] = fail_n;
        j["errors_by_class"] = by_class;
        json arr = json::array();
        for (const auto& r : results) arr.push_back(ItemToJson(r));
        j["results"] = arr;
        std::printf("%s\n", j.dump(2).c_str());
    } else {
        std::printf("probe: run_id=%s workload_hash=%s files=%zu\n", wl.run_id.c_str(),
                    validated.workload_hash.c_str(), results.size());
        if (!opts.skip_auth_check) {
            std::printf("auth: proxy=%s remaining=%.0fs\n", auth.proxy_path.c_str(),
                        auth.remaining_s);
        } else {
            std::printf("auth: skipped (--skip-auth-check)\n");
        }
        for (const auto& r : results) PrintHuman(r);
        std::printf("summary: ok=%" PRIu64 " failed=%" PRIu64 "\n", ok_n, fail_n);
        for (const auto& e : by_class) {
            std::printf("  %s: %" PRIu64 "\n", e.first.c_str(), e.second);
        }
    }

    if (admitted != work.size()) {
        std::fprintf(stderr, "probe: incomplete admission (%zu/%zu)\n", admitted, work.size());
        return 1;
    }
    return fail_n == 0 ? 0 : 1;
}

}  // namespace readgen
