#include "readgen/run_command.hh"

#include "readgen/build_info.hh"
#include "readgen/error_classifier.hh"
#include "readgen/file_session.hh"
#include "readgen/file_sink.hh"
#include "readgen/inflight.hh"
#include "readgen/metrics.hh"
#include "readgen/pushgateway_sink.hh"
#include "readgen/scheduler.hh"
#include "readgen/site_map.hh"
#include "readgen/sitename_resolver.hh"
#include "readgen/soft_fault_log.hh"
#include "readgen/token_bucket.hh"
#include "readgen/units.hh"
#include "readgen/xrdcl_env.hh"

#include <XrdVersion.hh>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef READGEN_VERSION
#define READGEN_VERSION "0.0.0"
#endif

namespace readgen {
namespace {

using Clock = std::chrono::steady_clock;

std::string DefaultJobId() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        if (buf[0] != '\0') return std::string(buf);
    }
    return "local";
}

// e.g. "500.00 MB/s (from '4Gbps')", or "uncapped".
std::string DescribeTargetRate(const RunConfig& cfg) {
    if (cfg.target_rate_bps == 0) return "uncapped";
    std::string s = FormatRate(cfg.target_rate_bps);
    if (!cfg.target_rate_input.empty()) s += " (from '" + cfg.target_rate_input + "')";
    return s;
}

void PrintDryRun(const RunConfig& cfg) {
    std::printf("run_id:         %s\n", cfg.run_id.c_str());
    std::printf("job_id:         %s\n", cfg.job_id.empty() ? "(hostname|local)" : cfg.job_id.c_str());
    std::printf("duration:       %s\n", FormatDuration(cfg.duration_s).c_str());
    std::printf("endpoint:       %s\n", cfg.endpoint.c_str());
    std::printf("filelist:       %s (%zu files)\n", cfg.filelist_path.c_str(), cfg.files.size());
    std::printf("target_rate:    %s\n", DescribeTargetRate(cfg).c_str());
    std::printf("workers:        %" PRIu32 "\n", cfg.workers);
    std::printf("pattern:        %s\n", PatternTypeName(cfg.pattern));
    std::printf("chunk_size:     %s\n", FormatBytes(cfg.chunk_size).c_str());
    if (cfg.pattern == PatternType::Vector || cfg.pattern == PatternType::Mixed) {
        std::printf("vector_chunks:  %" PRIu16 "\n", cfg.vector_chunks);
        if (cfg.pattern == PatternType::Mixed)
            std::printf("vector_fraction:%.2f\n", cfg.vector_fraction);
    }
    std::printf("file_fraction:  %.3f\n", cfg.file_fraction);
    if (cfg.max_bytes > 0) {
        std::printf("max_bytes:      %s%s\n", FormatBytes(cfg.max_bytes).c_str(),
                    cfg.max_bytes_auto ? " (auto)" : "");
    } else {
        std::printf("max_bytes:      (none)\n");
    }
    std::printf("session_timeout:%s\n",
                cfg.session_timeout_s > 0 ? FormatDuration(cfg.session_timeout_s).c_str() : "off");
    std::printf("conn_window:    %d s (XrdCl)\n", cfg.connection_window_s);
    std::printf("seed:           %" PRIu64 "\n", cfg.seed);
    std::printf("results_dir:    %s\n", cfg.write_results ? cfg.results_dir.c_str() : "(disabled)");
    std::printf("snapshot:       %s\n", FormatDuration(cfg.snapshot_interval_s).c_str());
    std::printf("pushgateway:    %s\n",
                cfg.pushgateway_url.empty() ? "(disabled)" : cfg.pushgateway_url.c_str());
    if (!cfg.pushgateway_url.empty()) {
        std::printf("push_job:       %s\n", cfg.pushgateway_job.c_str());
    }
    std::printf("site_map:       %s\n",
                cfg.site_map_path.empty() ? "(none)" : cfg.site_map_path.c_str());
    std::printf("sitename_query: %s\n", cfg.sitename_query ? "on" : "off");
    std::printf("dry_run:        true (no I/O)\n");
    if (!cfg.files.empty()) {
        std::printf("example URL:    %s\n", JoinUrl(cfg.endpoint, cfg.files.front()).c_str());
    }
}

int RunEngine(const RunConfig& cfg) {
    ApplyXrdClTimeouts(cfg.connection_window_s, cfg.connection_retry, cfg.request_timeout_s,
                       cfg.session_timeout_s);

    SiteMap site_map;
    const SiteMap* site_map_ptr = nullptr;
    if (!cfg.site_map_path.empty()) {
        try {
            site_map = SiteMap::LoadFile(cfg.site_map_path);
            site_map_ptr = &site_map;
            std::fprintf(stderr, "site_map: loaded %zu entries from %s\n", site_map.size(),
                         cfg.site_map_path.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
    }

    SitenameResolver sitename_resolver;
    if (cfg.sitename_query) {
        std::fprintf(stderr,
                     "sitename_query: on (background thread; independent of I/O and scheduling)\n");
    }

    // Burst covers a full worker pipeline so the rate limiter can admit a full
    // set of in-flight session charges without waiting on a 1-charge refill.
    const uint64_t burst = ComputeBucketBurst(cfg);
    TokenBucket bucket(cfg.target_rate_bps, burst);
    InFlightSemaphore inflight(cfg.workers);
    Scheduler sched(cfg);

    MetricsRegistry registry;
    const std::string job_id = cfg.job_id.empty() ? DefaultJobId() : cfg.job_id;
    registry.SetLabels(cfg.run_id, job_id, cfg.target, cfg.endpoint);
    registry.SetConfigGauges(cfg.target_rate_bps, cfg.workers);

    // Never call sync XrdCl Query from a session completion callback — it deadlocks
    // the client event loop (achieved rate sticks at 0). Resolve on the background
    // site thread instead.
    auto fill_cms_sites = [&] {
        const auto missing = registry.DataServersMissingSite();
        for (const auto& ds : missing) {
            std::string site;
            if (cfg.sitename_query) site = sitename_resolver.Resolve(ds);
            if (site.empty() && site_map_ptr != nullptr) site = site_map_ptr->Lookup(ds);
            if (!site.empty()) registry.SetCmsSite(ds, site);
        }
    };
    SoftFaultLogOut::Install(&registry);
    struct SoftFaultGuard {
        ~SoftFaultGuard() { SoftFaultLogOut::TearDown(); }
    } soft_fault_guard;

    std::unique_ptr<FileSink> sink;
    std::unique_ptr<PushgatewaySink> push;
    double cpu_at_start = 0.0;
    if (cfg.write_results) {
        RunInfoMeta meta;
        meta.version = READGEN_VERSION;
        meta.arch = BuildArch();
        meta.xrdcl_version = XrdVERSION;
        meta.seed = cfg.seed;
        meta.pattern = PatternTypeName(cfg.pattern);
        meta.schema_version = cfg.schema_version;
        meta.auth_mode = cfg.auth_mode;
        meta.workload_hash = cfg.workload_hash;
        meta.workload_resolved_json = cfg.workload_resolved_json;
        sink = std::make_unique<FileSink>(cfg.results_dir, cfg.run_id, std::move(meta));
        try {
            sink->Start();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
        std::fprintf(stderr, "results: %s\n", sink->run_dir().c_str());
    }
    if (!cfg.pushgateway_url.empty()) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        try {
            push = std::make_unique<PushgatewaySink>(cfg.pushgateway_url, cfg.pushgateway_job);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
        std::fprintf(stderr, "pushgateway: %s (job=%s instance=%s)\n", cfg.pushgateway_url.c_str(),
                     cfg.pushgateway_job.c_str(), job_id.c_str());
    }
    if (sink || push) {
        registry.SampleProc();
        cpu_at_start = SampleProcess().cpu_seconds;
    }

    std::atomic<uint64_t> live{0};
    std::atomic<uint64_t> fail_count{0};
    std::atomic<uint64_t> wall_timeout_count{0};
    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::duration_cast<Clock::duration>(
                                   std::chrono::duration<double>(cfg.duration_s));

    std::fprintf(stderr,
                 "run %s: duration=%s rate=%s workers=%" PRIu32
                 " pattern=%s files=%zu burst=%s\n",
                 cfg.run_id.c_str(), FormatDuration(cfg.duration_s).c_str(),
                 DescribeTargetRate(cfg).c_str(), cfg.workers, PatternTypeName(cfg.pattern),
                 cfg.files.size(), FormatBytes(burst).c_str());

    auto take_snapshot = [&] {
        const auto now = Clock::now();
        registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
        const double wall = std::chrono::duration<double>(now - t0).count();
        if (sink || push) registry.SampleProc();
        auto snap = registry.Snapshot(wall);
        if (sink) sink->WriteSnapshot(snap);
        if (push) {
            if (!push->Push(snap)) {
                std::fprintf(stderr, "warning: pushgateway push failed\n");
            }
        }
    };

    // Dedicated timer so snapshots continue even if admission is blocked on the
    // token bucket or a full inflight set.
    std::atomic<bool> stop_timer{false};
    std::thread timer([&] {
        const auto interval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(cfg.snapshot_interval_s, 0.1)));
        auto next = t0 + interval;
        while (!stop_timer.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (now < next) {
                std::this_thread::sleep_for(
                    std::min(std::chrono::duration_cast<std::chrono::milliseconds>(next - now),
                             std::chrono::milliseconds(50)));
                continue;
            }
            take_snapshot();
            next = Clock::now() + interval;
        }
    });

    // Sitename queries run on their own thread — never on XrdCl callbacks (deadlock)
    // and never on the snapshot timer (would delay Pushgateway pushes).
    std::atomic<bool> stop_sites{false};
    std::thread site_thread;
    if (cfg.sitename_query || site_map_ptr != nullptr) {
        site_thread = std::thread([&] {
            while (!stop_sites.load(std::memory_order_acquire)) {
                fill_cms_sites();
                for (int i = 0; i < 20 && !stop_sites.load(std::memory_order_acquire); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    }

    while (Clock::now() < deadline) {
        if (!inflight.AcquireUntil(deadline)) break;

        WorkItem work = sched.Next();
        if (!bucket.AcquireUntil(work.charge_bytes, deadline)) {
            inflight.Release();
            break;
        }

        const uint64_t charged = work.charge_bytes;
        live.fetch_add(1, std::memory_order_relaxed);
        registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());

        StartFileSession(work.session, [&, charged](FileSessionResult result) {
            // Do not Query sitename here — sync XrdCl from this callback deadlocks.
            if (result.ok) {
                registry.ObserveSessionOk(result.bytes_read, result.ops, result.open_ms / 1000.0,
                                          result.ttfb_ms / 1000.0, result.read_s,
                                          static_cast<double>(result.open_hosts),
                                          result.data_server);
            } else {
                const ErrorClass cls =
                    ClassifyXRootDError(result.status_code, result.err_code, result.error);
                registry.ObserveSessionFail(cls, result.data_server);
                // Log the first few failures, plus the first few wall timeouts
                // even if other error kinds already used up the budget.
                const uint64_t nth_fail = fail_count.fetch_add(1, std::memory_order_relaxed) + 1;
                const uint64_t nth_timeout =
                    result.timed_out
                        ? wall_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1
                        : 0;
                if (nth_fail <= 5 || (result.timed_out && nth_timeout <= 3)) {
                    // url is the request URL (often the global redirector + LFN). Log the
                    // resolved DataServer host and basename only — not the full path.
                    const char* ds =
                        result.data_server.empty() ? "(unknown)" : result.data_server.c_str();
                    std::string file = "(unknown)";
                    if (!result.url.empty()) {
                        const auto slash = result.url.rfind('/');
                        file = slash == std::string::npos ? result.url : result.url.substr(slash + 1);
                        if (file.empty()) file = "(unknown)";
                    }
                    std::fprintf(stderr,
                                 "session error: %s data_server=%s file=%s "
                                 "bytes_read=%" PRIu64 " ops=%" PRIu64 " open_ms=%.0f total_s=%.1f\n",
                                 result.error.c_str(), ds, file.c_str(), result.bytes_read,
                                 result.ops, result.open_ms, result.total_s);
                }
            }

            if (result.bytes_read > charged) {
                const uint64_t extra = result.bytes_read - charged;
                (void)bucket.TryAcquire(extra);
            } else if (result.bytes_read < charged) {
                bucket.Refund(charged - result.bytes_read);
            }

            live.fetch_sub(1, std::memory_order_relaxed);
            registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
            inflight.Release();
        });
    }

    // Drain wait: see kDrainWaitCapSec / kDrainTimeoutGraceSec in run_config.hh.
    double drain_cap_s = kDrainWaitCapSec;
    if (cfg.session_timeout_s > 0) {
        drain_cap_s = std::min(drain_cap_s, cfg.session_timeout_s + kDrainTimeoutGraceSec);
    }
    const auto drain_deadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(drain_cap_s));
    while (live.load(std::memory_order_relaxed) > 0 && Clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (live.load(std::memory_order_relaxed) > 0) {
        std::fprintf(stderr, "warning: drain timed out with %" PRIu64 " session(s) still live\n",
                     live.load(std::memory_order_relaxed));
    }

    stop_timer.store(true, std::memory_order_release);
    if (timer.joinable()) timer.join();

    stop_sites.store(true, std::memory_order_release);
    if (site_thread.joinable()) site_thread.join();

    const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    if (sink || push) registry.SampleProc();
    registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
    fill_cms_sites();
    const MetricsSnapshot final_snap = registry.Snapshot(elapsed);

    if (sink) {
        try {
            sink->WriteSnapshot(final_snap);
            sink->WriteResult(final_snap, cpu_at_start);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: results write failed: %s\n", e.what());
        }
    }
    if (push) {
        if (!push->Push(final_snap)) {
            std::fprintf(stderr, "warning: final pushgateway push failed\n");
        }
        if (!cfg.pushgateway_keep) {
            push->Finish(job_id);
        } else {
            std::fprintf(stderr, "pushgateway: keeping group job=%s instance=%s\n",
                         cfg.pushgateway_job.c_str(), job_id.c_str());
        }
    }

    std::printf("=== run summary ===\n");
    std::printf("run_id:         %s\n", cfg.run_id.c_str());
    std::printf("job_id:         %s\n", job_id.c_str());
    std::printf("elapsed:        %.3f s\n", elapsed);
    std::printf("sessions:       %" PRIu64 " ok / %" PRIu64 " fail\n", final_snap.sessions_ok,
                final_snap.sessions_fail);
    std::printf("bytes:          %s (%" PRIu64 ")\n",
                FormatBytes(final_snap.bytes_read_total).c_str(), final_snap.bytes_read_total);
    std::printf("ops:            %" PRIu64 "\n", final_snap.read_ops_total);
    std::printf("achieved:       %s\n",
                FormatRate(static_cast<uint64_t>(final_snap.achieved_rate_bytes)).c_str());
    if (cfg.target_rate_bps) {
        std::printf("target:         %s\n", DescribeTargetRate(cfg).c_str());
    }
    std::printf("inflight peak:  %" PRIu32 " / %" PRIu32 "\n", inflight.peak(), inflight.max());
    if (sink) std::printf("results:        %s\n", sink->run_dir().c_str());
    if (push) std::printf("pushgateway:    %s\n", cfg.pushgateway_url.c_str());
    if (!final_snap.errors_by_class.empty()) {
        std::printf("errors:\n");
        for (const auto& e : final_snap.errors_by_class) {
            std::printf("  %-14s %" PRIu64 "\n", e.first.c_str(), e.second);
        }
    }
    if (!final_snap.soft_faults_by_kind.empty()) {
        std::printf("soft_faults:\n");
        for (const auto& e : final_snap.soft_faults_by_kind) {
            std::printf("  %-14s %" PRIu64 "\n", e.first.c_str(), e.second);
        }
    }

    // Console summary is by CMS site. Per-DataServer detail stays in FileSink + D2.
    if (!final_snap.by_cms_site.empty()) {
        std::printf("by_cms_site:\n");
        for (const auto& kv : final_snap.by_cms_site) {
            const SiteStats& site = kv.second;
            std::printf("  %-24s bytes=%-12s ok=%" PRIu64 " fail=%" PRIu64 "\n",
                        site.cms_site.c_str(), FormatBytes(site.bytes_read).c_str(),
                        site.sessions_ok, site.sessions_fail);
        }
    }

    uint64_t unmapped_bytes = 0;
    uint64_t unmapped_ok = 0;
    uint64_t unmapped_fail = 0;
    size_t unmapped_servers = 0;
    for (const auto& kv : final_snap.by_data_server) {
        if (!kv.second.cms_site.empty()) continue;
        ++unmapped_servers;
        unmapped_bytes += kv.second.bytes_read;
        unmapped_ok += kv.second.sessions_ok;
        unmapped_fail += kv.second.sessions_fail;
    }
    if (unmapped_servers > 0) {
        std::printf("unmapped:       %zu data_server(s) bytes=%s ok=%" PRIu64 " fail=%" PRIu64
                    " (detail: result.json by_data_server / Grafana D2)\n",
                    unmapped_servers, FormatBytes(unmapped_bytes).c_str(), unmapped_ok,
                    unmapped_fail);
        if (cfg.site_map_path.empty() && !cfg.sitename_query) {
            std::printf("hint:          enable sitename query (default) or --site-map for by_cms_site\n");
        } else if (cfg.site_map_path.empty()) {
            std::printf("hint:          some servers omit sitename; optional --site-map for overrides\n");
        }
    } else if (final_snap.by_cms_site.empty() && !final_snap.by_data_server.empty()) {
        std::printf("data_servers:   %zu (no cms_site map hits; detail in result.json / D2)\n",
                    final_snap.by_data_server.size());
    }

    // Join the shared deadline thread before process teardown — a detached
    // forever loop races static destruction and SEGV's on exit.
    ShutdownSessionWatchdog();

    return final_snap.sessions_fail > 0 && final_snap.sessions_ok == 0 ? 1 : 0;
}

}  // namespace

int RunRunCommand(const RunConfig& cfg_in) {
    RunConfig cfg = cfg_in;
    try {
        ResolveRunConfig(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    if (cfg.endpoint.empty()) {
        std::fprintf(stderr, "error: --endpoint is required\n");
        return 2;
    }
    if (cfg.files.empty()) {
        std::fprintf(stderr, "error: filelist is empty\n");
        return 2;
    }
    if (cfg.dry_run) {
        PrintDryRun(cfg);
        return 0;
    }
    if (cfg.max_bytes_auto) {
        std::fprintf(stderr, "max_bytes auto → %s (rate/workers × %.0fs, capped at %s)\n",
                     FormatBytes(cfg.max_bytes).c_str(), kAutoMaxAmortizeSec,
                     FormatBytes(kAutoMaxHardCapBytes).c_str());
    }
    return RunEngine(cfg);
}

}  // namespace readgen
