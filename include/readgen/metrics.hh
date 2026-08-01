#ifndef READGEN_METRICS_HH
#define READGEN_METRICS_HH

#include "readgen/error_classifier.hh"
#include "readgen/site_map.hh"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace readgen {

// Coarse kinds produced by ClassifySoftFaultMessage. "other" must stay last
// (fallback index for unrecognized kinds).
inline constexpr std::array<const char*, 6> kSoftFaultKinds = {
    "connection", "timeout", "tls_auth", "io", "redirect", "other",
};

// Fixed log-spaced upper bounds (seconds), 1 ms → 30 s, plus +Inf.
// redirects_per_open reuses the same index layout with unitless values.
inline constexpr std::array<double, 15> kLatencyBucketBounds = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0,
};

inline constexpr size_t kHistogramBuckets = kLatencyBucketBounds.size() + 1;  // +Inf

// Sentinel key when FileSessionResult::data_server is empty.
inline constexpr const char* kUnknownDataServer = "unknown";

struct HistogramSnapshot {
    std::vector<double> bounds;       // finite upper bounds (excludes +Inf)
    std::vector<uint64_t> counts;     // non-cumulative per-bucket (+Inf last)
    uint64_t count = 0;
    double sum = 0.0;
};

// Derive percentile from non-cumulative bucket counts (p in [0, 1]).
double HistogramPercentile(const HistogramSnapshot& h, double p);

// Per resolved data server (and optional CMS site) attribution.
struct EndpointStats {
    std::string data_server;
    std::string cms_site;  // empty if unmapped
    uint64_t bytes_read = 0;
    uint64_t sessions_ok = 0;
    uint64_t sessions_fail = 0;
    std::map<std::string, uint64_t> errors_by_class;
};

// Rollup of EndpointStats that share the same cms_site (mapped only).
struct SiteStats {
    std::string cms_site;
    uint64_t bytes_read = 0;
    uint64_t sessions_ok = 0;
    uint64_t sessions_fail = 0;
    std::map<std::string, uint64_t> errors_by_class;
};

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
    // Cumulative achieved rate (bytes / wall_s). Matches run-summary MB/s.
    double achieved_rate_bytes = 0.0;

    HistogramSnapshot open_seconds;
    HistogramSnapshot ttfb_seconds;
    HistogramSnapshot read_seconds;
    HistogramSnapshot redirects_per_open;

    std::map<std::string, uint64_t> errors_by_class;
    std::map<std::string, uint64_t> soft_faults_by_kind;

    // Independent keys — no top-N / "other" collapsing.
    std::map<std::string, EndpointStats> by_data_server;
    std::map<std::string, SiteStats> by_cms_site;

    uint64_t inflight_reads = 0;
    uint64_t peak_inflight = 0;
    uint32_t max_inflight = 0;

    double cpu_seconds_total = 0.0;
    uint64_t process_resident_memory_bytes = 0;
};

// Lock-free fixed-bucket histogram. Observe is safe from XrdCl callback threads.
class Histogram {
public:
    void Observe(double value);
    HistogramSnapshot Snapshot() const;

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
    void SetConfigGauges(uint64_t target_rate_bps, uint32_t max_inflight);

    // Optional CMS site map for clean hostname→site attribution. Not owned.
    void SetSiteMap(const SiteMap* map);

    void ObserveSessionOk(uint64_t bytes, uint64_t ops, double open_s, double ttfb_s, double read_s,
                          double redirects, const std::string& data_server = {},
                          const std::string& cms_site = {});
    void ObserveSessionFail(ErrorClass error_class, const std::string& data_server = {},
                            const std::string& cms_site = {});
    // XrdCl Error-level log lines (may not fail a session — soft faults).
    // `kind` should come from ClassifySoftFaultMessage; unknown kinds count as "other".
    void ObserveSoftFault(const std::string& kind);
    void SetInflight(uint64_t live, uint64_t peak);

    // Attach/replace cms_site for an already-seen data_server (safe off the XrdCl
    // callback path — used after deferred sitename queries).
    void SetCmsSite(const std::string& data_server, const std::string& cms_site);

    // DataServers recorded so far with empty cms_site (for deferred resolution).
    std::vector<std::string> DataServersMissingSite() const;

    // Refresh CPU/RSS from /proc (call on snapshot thread).
    void SampleProc();

    // Build a snapshot. wall_s is steady_clock elapsed time.
    MetricsSnapshot Snapshot(double wall_s);

private:
    void AttributeLocked(const std::string& data_server, bool ok, uint64_t bytes, ErrorClass error_class,
                         const std::string& cms_site);

    std::string run_id_;
    std::string job_id_;
    std::string target_;
    std::string endpoint_;
    const SiteMap* site_map_ = nullptr;

    std::atomic<uint64_t> bytes_read_total_{0};
    std::atomic<uint64_t> sessions_ok_{0};
    std::atomic<uint64_t> sessions_fail_{0};
    std::atomic<uint64_t> read_ops_total_{0};
    std::atomic<uint64_t> target_rate_bytes_{0};
    std::atomic<uint32_t> max_inflight_{0};
    std::atomic<uint64_t> inflight_reads_{0};
    std::atomic<uint64_t> peak_inflight_{0};

    Histogram open_seconds_;
    Histogram ttfb_seconds_;
    Histogram read_seconds_;
    Histogram redirects_per_open_;

    std::array<std::atomic<uint64_t>, kErrorClassCount> errors_by_class_{};
    std::array<std::atomic<uint64_t>, kSoftFaultKinds.size()> soft_faults_by_kind_{};

    std::atomic<uint64_t> cpu_us_{0};  // process CPU as microseconds
    std::atomic<uint64_t> rss_bytes_{0};

    mutable std::mutex endpoint_mu_;
    // data_server → counters (cms_site filled when map hits).
    std::map<std::string, EndpointStats> by_data_server_;
};

}  // namespace readgen

#endif  // READGEN_METRICS_HH
