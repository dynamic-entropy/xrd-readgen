#ifndef READGEN_METRICS_HH
#define READGEN_METRICS_HH

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace readgen {

// Fixed log-spaced upper bounds (seconds), 1 ms → 30 s, plus +Inf.
// redirects_per_open reuses the same index layout with unitless values.
inline constexpr std::array<double, 15> kLatencyBucketBounds = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0,
};

inline constexpr size_t kHistogramBuckets = kLatencyBucketBounds.size() + 1;  // +Inf

struct HistogramSnapshot {
    std::vector<double> bounds;       // finite upper bounds (excludes +Inf)
    std::vector<uint64_t> counts;     // non-cumulative per-bucket (+Inf last)
    uint64_t count = 0;
    double sum = 0.0;
};

// Derive percentile from non-cumulative bucket counts (p in [0, 1]).
double HistogramPercentile(const HistogramSnapshot& h, double p);

struct MetricsSnapshot {
    std::string run_id;
    std::string job_id;
    std::string target;
    std::string endpoint;
    double wall_s = 0.0;

    uint64_t bytes_read_total = 0;
    uint64_t sessions_ok = 0;
    uint64_t sessions_fail = 0;
    uint64_t read_ops_total = 0;
    double target_rate_bytes = 0.0;
    // Instantaneous achieved rate over the last snapshot interval (bytes/s),
    // using the same steady_clock elapsed time as wall_s / run duration.
    // Prefer this over Prometheus rate(counter) when scraping Pushgateway.
    double achieved_rate_bytes = 0.0;

    HistogramSnapshot open_seconds;
    HistogramSnapshot ttfb_seconds;
    HistogramSnapshot read_seconds;
    HistogramSnapshot redirects_per_open;

    std::map<std::string, uint64_t> errors_by_class;
    std::map<std::string, uint64_t> soft_faults_by_kind;

    uint64_t inflight_reads = 0;
    uint64_t peak_inflight = 0;
    uint32_t workers_configured = 0;

    double cpu_seconds_total = 0.0;
    uint64_t process_resident_memory_bytes = 0;
};

// Lock-free fixed-bucket histogram. Observe is safe from XrdCl callback threads.
class Histogram {
public:
    void Observe(double value);
    HistogramSnapshot Snapshot() const;
    void Reset();

private:
    std::array<std::atomic<uint64_t>, kHistogramBuckets> buckets_{};
    std::atomic<uint64_t> count_{0};
    // Sum stored as fixed-point microseconds to stay lock-free.
    std::atomic<uint64_t> sum_us_{0};
};

struct ProcessSample {
    double cpu_seconds = 0.0;
    uint64_t rss_bytes = 0;
};

// Sample /proc/self CPU (utime+stime) and RSS. Returns zeros if unavailable.
ProcessSample SampleProcess();

class MetricsRegistry {
public:
    void SetLabels(std::string run_id, std::string job_id, std::string target, std::string endpoint);
    void SetConfigGauges(uint64_t target_rate_bps, uint32_t workers);

    void ObserveSessionOk(uint64_t bytes, uint64_t ops, double open_s, double ttfb_s, double read_s,
                          double redirects);
    void ObserveSessionFail(const std::string& error_class);
    // XrdCl Error-level log lines (may not fail a session — soft faults).
    void ObserveSoftFault(const std::string& kind);
    void SetInflight(uint64_t live, uint64_t peak);

    // Refresh CPU/RSS from /proc (call on snapshot thread).
    void SampleProc();

    // Build a snapshot. Non-const: updates interval achieved_rate_bytes from the
    // previous Snapshot() call using wall_s (steady elapsed) and bytes counters.
    MetricsSnapshot Snapshot(double wall_s);

    const std::string& run_id() const { return run_id_; }
    const std::string& job_id() const { return job_id_; }

private:
    std::string run_id_;
    std::string job_id_;
    std::string target_;
    std::string endpoint_;

    std::atomic<uint64_t> bytes_read_total_{0};
    std::atomic<uint64_t> sessions_ok_{0};
    std::atomic<uint64_t> sessions_fail_{0};
    std::atomic<uint64_t> read_ops_total_{0};
    std::atomic<uint64_t> target_rate_bytes_{0};
    std::atomic<uint32_t> workers_configured_{0};
    std::atomic<uint64_t> inflight_reads_{0};
    std::atomic<uint64_t> peak_inflight_{0};

    Histogram open_seconds_;
    Histogram ttfb_seconds_;
    Histogram read_seconds_;
    Histogram redirects_per_open_;

    // Fixed set of error-class counters (names match ErrorClassName).
    std::atomic<uint64_t> err_auth_{0};
    std::atomic<uint64_t> err_timeout_{0};
    std::atomic<uint64_t> err_connection_{0};
    std::atomic<uint64_t> err_server_error_{0};
    std::atomic<uint64_t> err_not_found_{0};
    std::atomic<uint64_t> err_client_error_{0};
    std::atomic<uint64_t> err_redirect_loop_{0};
    std::atomic<uint64_t> err_unknown_{0};

    std::atomic<uint64_t> soft_connection_{0};
    std::atomic<uint64_t> soft_timeout_{0};
    std::atomic<uint64_t> soft_tls_auth_{0};
    std::atomic<uint64_t> soft_io_{0};
    std::atomic<uint64_t> soft_redirect_{0};
    std::atomic<uint64_t> soft_other_{0};

    std::atomic<uint64_t> cpu_us_{0};  // process CPU as microseconds
    std::atomic<uint64_t> rss_bytes_{0};

    // Last Snapshot() sample for interval achieved_rate_bytes (main thread only).
    bool have_rate_sample_ = false;
    double last_rate_wall_s_ = 0.0;
    uint64_t last_rate_bytes_ = 0;
};

}  // namespace readgen

#endif  // READGEN_METRICS_HH
