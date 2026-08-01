#include "readgen/metrics.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace readgen {
namespace {

size_t BucketIndex(double value) {
    for (size_t i = 0; i < kLatencyBucketBounds.size(); ++i) {
        if (value <= kLatencyBucketBounds[i]) return i;
    }
    return kHistogramBuckets - 1;  // +Inf
}

}  // namespace

double HistogramPercentile(const HistogramSnapshot& h, double p) {
    if (h.count == 0 || h.counts.empty()) return 0.0;
    p = std::clamp(p, 0.0, 1.0);
    const double target = p * static_cast<double>(h.count);
    uint64_t cum = 0;
    const size_t n = h.counts.size();
    for (size_t i = 0; i < n; ++i) {
        const uint64_t prev = cum;
        cum += h.counts[i];
        if (static_cast<double>(cum) < target) continue;

        const double lower = (i == 0) ? 0.0 : h.bounds[i - 1];
        const double upper =
            (i < h.bounds.size()) ? h.bounds[i] : (h.bounds.empty() ? 0.0 : h.bounds.back() * 2.0);
        if (h.counts[i] == 0) return upper;
        const double frac =
            (target - static_cast<double>(prev)) / static_cast<double>(h.counts[i]);
        return lower + frac * (upper - lower);
    }
    return h.bounds.empty() ? 0.0 : h.bounds.back();
}

void Histogram::Observe(double value) {
    if (value < 0.0 || !std::isfinite(value)) value = 0.0;
    const auto us = static_cast<uint64_t>(value * 1e6);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    buckets_[BucketIndex(value)].fetch_add(1, std::memory_order_relaxed);
}

HistogramSnapshot Histogram::Snapshot() const {
    HistogramSnapshot s;
    s.bounds.assign(kLatencyBucketBounds.begin(), kLatencyBucketBounds.end());
    s.counts.resize(kHistogramBuckets);
    for (size_t i = 0; i < kHistogramBuckets; ++i) {
        s.counts[i] = buckets_[i].load(std::memory_order_relaxed);
    }
    s.count = count_.load(std::memory_order_relaxed);
    s.sum = static_cast<double>(sum_us_.load(std::memory_order_relaxed)) / 1e6;
    return s;
}

ProcessSample SampleProcess() {
    ProcessSample out;
    const long ticks = sysconf(_SC_CLK_TCK);
    const long page = sysconf(_SC_PAGESIZE);
    if (ticks <= 0 || page <= 0) return out;

    std::ifstream stat("/proc/self/stat");
    if (!stat) return out;
    std::string line;
    if (!std::getline(stat, line)) return out;

    // Format: pid (comm) state ppid ... utime stime ...
    // Skip to after the last ')' of comm, then skip state, then count fields.
    const auto rparen = line.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= line.size()) return out;
    const char* p = line.c_str() + rparen + 2;
    while (*p == ' ') ++p;
    // Field 3: state (non-numeric)
    while (*p && *p != ' ') ++p;
    // Fields 4..13: skip 10 tokens; fields 14/15 are utime/stime.
    unsigned long utime = 0, stime = 0;
    for (int i = 0; i < 12; ++i) {
        while (*p == ' ') ++p;
        char* end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 10);
        if (end == p) return out;
        if (i == 10) utime = v;
        if (i == 11) stime = v;
        p = end;
    }
    out.cpu_seconds = static_cast<double>(utime + stime) / static_cast<double>(ticks);

    std::ifstream statm("/proc/self/statm");
    if (statm) {
        uint64_t size = 0, resident = 0;
        if (statm >> size >> resident) {
            out.rss_bytes = resident * static_cast<uint64_t>(page);
        }
    }
    return out;
}

void MetricsRegistry::SetLabels(std::string run_id, std::string job_id, std::string target,
                                std::string endpoint) {
    run_id_ = std::move(run_id);
    job_id_ = std::move(job_id);
    target_ = std::move(target);
    endpoint_ = std::move(endpoint);
}

void MetricsRegistry::SetConfigGauges(uint64_t target_rate_bytes_per_s, uint32_t max_inflight) {
    target_rate_bytes_.store(target_rate_bytes_per_s, std::memory_order_relaxed);
    max_inflight_.store(max_inflight, std::memory_order_relaxed);
}

void MetricsRegistry::SetSiteMap(const SiteMap* map) { site_map_ = map; }

void MetricsRegistry::AttributeLocked(const std::string& data_server, bool ok, uint64_t bytes,
                                      ErrorClass error_class, const std::string& cms_site) {
    const std::string key = data_server.empty() ? kUnknownDataServer : data_server;
    EndpointStats& ep = by_data_server_[key];
    ep.data_server = key;
    if (ep.cms_site.empty()) {
        if (!cms_site.empty()) {
            ep.cms_site = cms_site;
        } else if (site_map_ != nullptr) {
            ep.cms_site = site_map_->Lookup(key);
        }
    }
    if (ok) {
        ep.bytes_read += bytes;
        ++ep.sessions_ok;
    } else {
        ++ep.sessions_fail;
        if (error_class != ErrorClass::None) {
            ep.errors_by_class[ErrorClassName(error_class)] += 1;
        }
    }
}

void MetricsRegistry::ObserveSessionOk(uint64_t bytes, uint64_t ops, double open_s, double ttfb_s,
                                       double read_s, double redirects, const std::string& data_server,
                                       const std::string& cms_site) {
    bytes_read_total_.fetch_add(bytes, std::memory_order_relaxed);
    sessions_ok_.fetch_add(1, std::memory_order_relaxed);
    read_ops_total_.fetch_add(ops, std::memory_order_relaxed);
    open_seconds_.Observe(open_s);
    ttfb_seconds_.Observe(ttfb_s);
    read_seconds_.Observe(read_s);
    redirects_per_open_.Observe(redirects);

    std::lock_guard<std::mutex> lock(endpoint_mu_);
    AttributeLocked(data_server, true, bytes, ErrorClass::None, cms_site);
}

void MetricsRegistry::ObserveSessionFail(ErrorClass error_class, const std::string& data_server,
                                         const std::string& cms_site) {
    sessions_fail_.fetch_add(1, std::memory_order_relaxed);
    errors_by_class_[static_cast<size_t>(error_class)].fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(endpoint_mu_);
    AttributeLocked(data_server, false, 0, error_class, cms_site);
}

void MetricsRegistry::ObserveSoftFault(const std::string& kind) {
    size_t idx = kSoftFaultKinds.size() - 1;  // "other"
    for (size_t i = 0; i + 1 < kSoftFaultKinds.size(); ++i) {
        if (kind == kSoftFaultKinds[i]) {
            idx = i;
            break;
        }
    }
    soft_faults_by_kind_[idx].fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::SetInflight(uint64_t live, uint64_t peak) {
    inflight_reads_.store(live, std::memory_order_relaxed);
    peak_inflight_.store(peak, std::memory_order_relaxed);
}

void MetricsRegistry::SetCmsSite(const std::string& data_server, const std::string& cms_site) {
    if (data_server.empty() || cms_site.empty()) return;
    const std::string key = data_server;
    std::lock_guard<std::mutex> lock(endpoint_mu_);
    auto it = by_data_server_.find(key);
    if (it == by_data_server_.end()) return;
    it->second.cms_site = cms_site;
}

std::vector<std::string> MetricsRegistry::DataServersMissingSite() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(endpoint_mu_);
    out.reserve(by_data_server_.size());
    for (const auto& kv : by_data_server_) {
        if (kv.second.cms_site.empty() && kv.first != kUnknownDataServer) {
            out.push_back(kv.first);
        }
    }
    return out;
}

void MetricsRegistry::SampleProc() {
    const ProcessSample s = SampleProcess();
    cpu_us_.store(static_cast<uint64_t>(s.cpu_seconds * 1e6), std::memory_order_relaxed);
    rss_bytes_.store(s.rss_bytes, std::memory_order_relaxed);
}

MetricsSnapshot MetricsRegistry::Snapshot(double wall_s) {
    MetricsSnapshot s;
    s.run_id = run_id_;
    s.job_id = job_id_;
    s.target = target_;
    s.endpoint = endpoint_;
    s.wall_s = wall_s;
    s.bytes_read_total = bytes_read_total_.load(std::memory_order_relaxed);
    s.sessions_ok = sessions_ok_.load(std::memory_order_relaxed);
    s.sessions_fail = sessions_fail_.load(std::memory_order_relaxed);
    s.read_ops_total = read_ops_total_.load(std::memory_order_relaxed);
    s.target_rate_bytes = static_cast<double>(target_rate_bytes_.load(std::memory_order_relaxed));
    s.open_seconds = open_seconds_.Snapshot();
    s.ttfb_seconds = ttfb_seconds_.Snapshot();
    s.read_seconds = read_seconds_.Snapshot();
    s.redirects_per_open = redirects_per_open_.Snapshot();
    s.inflight_reads = inflight_reads_.load(std::memory_order_relaxed);
    s.peak_inflight = peak_inflight_.load(std::memory_order_relaxed);
    s.max_inflight = max_inflight_.load(std::memory_order_relaxed);
    s.cpu_seconds_total = static_cast<double>(cpu_us_.load(std::memory_order_relaxed)) / 1e6;
    s.process_resident_memory_bytes = rss_bytes_.load(std::memory_order_relaxed);

    if (s.wall_s > 0.0) {
        s.achieved_rate_bytes = static_cast<double>(s.bytes_read_total) / s.wall_s;
    }

    for (size_t i = 0; i < errors_by_class_.size(); ++i) {
        const uint64_t v = errors_by_class_[i].load(std::memory_order_relaxed);
        if (v > 0) s.errors_by_class[ErrorClassName(static_cast<ErrorClass>(i))] = v;
    }
    for (size_t i = 0; i < soft_faults_by_kind_.size(); ++i) {
        const uint64_t v = soft_faults_by_kind_[i].load(std::memory_order_relaxed);
        if (v > 0) s.soft_faults_by_kind[kSoftFaultKinds[i]] = v;
    }

    {
        std::lock_guard<std::mutex> lock(endpoint_mu_);
        s.by_data_server = by_data_server_;
    }
    for (const auto& kv : s.by_data_server) {
        const EndpointStats& ep = kv.second;
        if (ep.cms_site.empty()) continue;
        SiteStats& site = s.by_cms_site[ep.cms_site];
        site.cms_site = ep.cms_site;
        site.bytes_read += ep.bytes_read;
        site.sessions_ok += ep.sessions_ok;
        site.sessions_fail += ep.sessions_fail;
        for (const auto& err : ep.errors_by_class) {
            site.errors_by_class[err.first] += err.second;
        }
    }
    return s;
}

}  // namespace readgen
