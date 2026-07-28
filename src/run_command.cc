#include "readgen/run_command.hh"

#include "readgen/error_classifier.hh"
#include "readgen/file_session.hh"
#include "readgen/file_sink.hh"
#include "readgen/inflight.hh"
#include "readgen/metrics.hh"
#include "readgen/pushgateway_sink.hh"
#include "readgen/scheduler.hh"
#include "readgen/soft_fault_log.hh"
#include "readgen/token_bucket.hh"
#include "readgen/units.hh"

#include <XrdVersion.hh>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef READGEN_VERSION
#define READGEN_VERSION "0.0.0"
#endif

namespace readgen {
namespace {

using Clock = std::chrono::steady_clock;

const char* BuildArch() {
#if defined(__aarch64__) || defined(__arm64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string DefaultJobId() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        if (buf[0] != '\0') return std::string(buf);
    }
    return "local";
}

void PrintDryRun(const RunConfig& cfg) {
    std::printf("run_id:         %s\n", cfg.run_id.c_str());
    std::printf("job_id:         %s\n", cfg.job_id.empty() ? "(hostname|local)" : cfg.job_id.c_str());
    std::printf("duration:       %s\n", FormatDuration(cfg.duration_s).c_str());
    std::printf("endpoint:       %s\n", cfg.endpoint.c_str());
    std::printf("filelist:       %s (%zu files)\n", cfg.filelist_path.c_str(), cfg.files.size());
    std::printf("target_rate:    %s\n",
                cfg.target_rate_bps ? FormatRate(cfg.target_rate_bps).c_str() : "uncapped");
    std::printf("workers:        %" PRIu32 " (max in-flight sessions)\n", cfg.workers);
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
    }
    std::printf("seed:           %" PRIu64 "\n", cfg.seed);
    std::printf("results_dir:    %s\n", cfg.write_results ? cfg.results_dir.c_str() : "(disabled)");
    std::printf("snapshot:       %s\n", FormatDuration(cfg.snapshot_interval_s).c_str());
    std::printf("pushgateway:    %s\n",
                cfg.pushgateway_url.empty() ? "(disabled)" : cfg.pushgateway_url.c_str());
    if (!cfg.pushgateway_url.empty()) {
        std::printf("push_job:       %s\n", cfg.pushgateway_job.c_str());
    }
    std::printf("dry_run:        true (no I/O)\n");
    if (!cfg.files.empty()) {
        std::printf("example URL:    %s\n", JoinUrl(cfg.endpoint, cfg.files.front()).c_str());
    }
}

struct RunStats {
    std::mutex mu;
    uint64_t sessions_ok = 0;
    uint64_t sessions_fail = 0;
    uint64_t bytes = 0;
    uint64_t ops = 0;
    std::map<ErrorClass, uint64_t> errors;
};

int RunEngine(const RunConfig& cfg) {
    // Bias the limiter slightly above --rate so open/redirect idle and soft
    // faults don't leave achieved chronically under target. Metrics still
    // report the configured target; overshoot of a few percent is intentional.
    constexpr double kRateOvershoot = 1.10;
    const uint64_t bucket_rate =
        cfg.target_rate_bps == 0
            ? 0
            : static_cast<uint64_t>(static_cast<double>(cfg.target_rate_bps) * kRateOvershoot);
    // Burst must cover one session charge; default 1s-of-rate is too small when
    // --max-bytes is large (Acquire would wait until deadline and transfer 0).
    // Extra headroom (2s of bucket rate) lets the pipeline catch up after slow opens.
    const uint64_t burst =
        bucket_rate == 0
            ? 0
            : std::max({bucket_rate * 2, cfg.max_bytes > 0 ? cfg.max_bytes : bucket_rate, bucket_rate});
    TokenBucket bucket(bucket_rate, burst);
    InFlightSemaphore inflight(cfg.workers);
    Scheduler sched(cfg);
    RunStats stats;

    MetricsRegistry registry;
    const std::string job_id = cfg.job_id.empty() ? DefaultJobId() : cfg.job_id;
    registry.SetLabels(cfg.run_id, job_id, cfg.target, cfg.endpoint);
    registry.SetConfigGauges(cfg.target_rate_bps, cfg.workers);
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
    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::duration_cast<Clock::duration>(
                                   std::chrono::duration<double>(cfg.duration_s));
    auto next_snap = t0 + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(cfg.snapshot_interval_s));

    std::fprintf(stderr, "run %s: duration=%s rate=%s workers=%" PRIu32 " pattern=%s files=%zu\n",
                 cfg.run_id.c_str(), FormatDuration(cfg.duration_s).c_str(),
                 cfg.target_rate_bps ? FormatRate(cfg.target_rate_bps).c_str() : "uncapped", cfg.workers,
                 PatternTypeName(cfg.pattern), cfg.files.size());

    auto maybe_snapshot = [&](bool force) {
        if (!sink && !push) return;
        const auto now = Clock::now();
        if (!force && now < next_snap) return;
        registry.SampleProc();
        registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
        const double wall = std::chrono::duration<double>(now - t0).count();
        const auto snap = registry.Snapshot(wall);
        if (sink) sink->WriteSnapshot(snap);
        if (push) {
            if (!push->Push(snap)) {
                std::fprintf(stderr, "warning: pushgateway push failed\n");
            }
        }
        next_snap = now + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(cfg.snapshot_interval_s));
    };

    while (Clock::now() < deadline) {
        maybe_snapshot(false);

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
            {
                std::lock_guard<std::mutex> lock(stats.mu);
                if (result.ok) {
                    ++stats.sessions_ok;
                    stats.bytes += result.bytes_read;
                    stats.ops += result.ops;
                    registry.ObserveSessionOk(result.bytes_read, result.ops, result.open_ms / 1000.0,
                                              result.ttfb_ms / 1000.0, result.read_s,
                                              static_cast<double>(result.open_hosts));
                } else {
                    ++stats.sessions_fail;
                    const ErrorClass cls =
                        ClassifyXRootDError(result.status_code, result.err_code, result.error);
                    ++stats.errors[cls];
                    registry.ObserveSessionFail(ErrorClassName(cls));
                    if (stats.sessions_fail <= 3) {
                        std::fprintf(stderr, "session error: %s\n", result.error.c_str());
                    }
                }

                if (result.bytes_read > charged) {
                    // Consume extra tokens without blocking (may briefly overshoot).
                    const uint64_t extra = result.bytes_read - charged;
                    (void)bucket.TryAcquire(extra);
                } else if (result.bytes_read < charged) {
                    bucket.Refund(charged - result.bytes_read);
                }
            }
            live.fetch_sub(1, std::memory_order_relaxed);
            registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
            inflight.Release();
        });
    }

    // Drain in-flight sessions.
    const auto drain_deadline = Clock::now() + std::chrono::minutes(10);
    while (live.load(std::memory_order_relaxed) > 0 && Clock::now() < drain_deadline) {
        maybe_snapshot(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    uint64_t bytes = 0, ops = 0, ok = 0, fail = 0;
    std::map<ErrorClass, uint64_t> errors;
    {
        std::lock_guard<std::mutex> lock(stats.mu);
        bytes = stats.bytes;
        ops = stats.ops;
        ok = stats.sessions_ok;
        fail = stats.sessions_fail;
        errors = stats.errors;
    }

    if (sink || push) {
        registry.SampleProc();
        registry.SetInflight(live.load(std::memory_order_relaxed), inflight.peak());
        const auto final_snap = registry.Snapshot(elapsed);
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
    }

    const double mib_s = elapsed > 0 ? bytes / elapsed / (1024.0 * 1024.0) : 0.0;
    const double target_mib_s = cfg.target_rate_bps / (1024.0 * 1024.0);

    std::printf("=== run summary ===\n");
    std::printf("run_id:         %s\n", cfg.run_id.c_str());
    std::printf("job_id:         %s\n", job_id.c_str());
    std::printf("elapsed:        %.3f s\n", elapsed);
    std::printf("sessions:       %" PRIu64 " ok / %" PRIu64 " fail\n", ok, fail);
    std::printf("bytes:          %s (%" PRIu64 ")\n", FormatBytes(bytes).c_str(), bytes);
    std::printf("ops:            %" PRIu64 "\n", ops);
    std::printf("achieved:       %.2f MiB/s\n", mib_s);
    if (cfg.target_rate_bps) std::printf("target:         %.2f MiB/s\n", target_mib_s);
    std::printf("inflight peak:  %" PRIu32 " / %" PRIu32 "\n", inflight.peak(), inflight.max());
    if (sink) std::printf("results:        %s\n", sink->run_dir().c_str());
    if (push) std::printf("pushgateway:    %s\n", cfg.pushgateway_url.c_str());
    if (!errors.empty()) {
        std::printf("errors:\n");
        for (const auto& e : errors) {
            std::printf("  %-14s %" PRIu64 "\n", ErrorClassName(e.first), e.second);
        }
    }
    {
        const auto soft = registry.Snapshot(elapsed).soft_faults_by_kind;
        if (!soft.empty()) {
            std::printf("soft_faults:\n");
            for (const auto& e : soft) {
                std::printf("  %-14s %" PRIu64 "\n", e.first.c_str(), e.second);
            }
        }
    }

    return fail > 0 && ok == 0 ? 1 : 0;
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
        std::fprintf(stderr, "max_bytes auto → %s (rate/workers × 8s, capped)\n",
                     FormatBytes(cfg.max_bytes).c_str());
    }
    return RunEngine(cfg);
}

}  // namespace readgen
