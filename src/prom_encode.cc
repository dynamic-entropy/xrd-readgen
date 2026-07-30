#include "readgen/prom_encode.hh"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace readgen {
namespace {

std::string EscapeLabel(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string CommonLabels(const MetricsSnapshot& s) {
    std::ostringstream o;
    o << "run_id=\"" << EscapeLabel(s.run_id) << "\","
      << "job_id=\"" << EscapeLabel(s.job_id) << "\","
      << "target=\"" << EscapeLabel(s.target) << "\","
      << "endpoint=\"" << EscapeLabel(s.endpoint) << "\"";
    return o.str();
}

void AppendCounter(std::ostringstream& o, const char* name, const char* help, const std::string& labels,
                   uint64_t value) {
    o << "# HELP " << name << ' ' << help << '\n';
    o << "# TYPE " << name << " counter\n";
    o << name << '{' << labels << "} " << value << '\n';
}

void AppendGauge(std::ostringstream& o, const char* name, const char* help, const std::string& labels,
                 double value) {
    o << "# HELP " << name << ' ' << help << '\n';
    o << "# TYPE " << name << " gauge\n";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    o << name << '{' << labels << "} " << buf << '\n';
}

void AppendGaugeU64(std::ostringstream& o, const char* name, const char* help, const std::string& labels,
                    uint64_t value) {
    o << "# HELP " << name << ' ' << help << '\n';
    o << "# TYPE " << name << " gauge\n";
    o << name << '{' << labels << "} " << value << '\n';
}

void AppendHistogram(std::ostringstream& o, const char* name, const char* help, const std::string& labels,
                     const HistogramSnapshot& h) {
    o << "# HELP " << name << ' ' << help << '\n';
    o << "# TYPE " << name << " histogram\n";

    uint64_t cum = 0;
    const size_t n_finite = h.bounds.size();
    for (size_t i = 0; i < h.counts.size(); ++i) {
        cum += h.counts[i];
        char le[64];
        if (i < n_finite) {
            std::snprintf(le, sizeof(le), "%.9g", h.bounds[i]);
        } else {
            std::snprintf(le, sizeof(le), "+Inf");
        }
        o << name << "_bucket{" << labels << ",le=\"" << le << "\"} " << cum << '\n';
    }
    // If counts were empty, still emit +Inf 0.
    if (h.counts.empty()) {
        o << name << "_bucket{" << labels << ",le=\"+Inf\"} 0\n";
    }

    char sumbuf[64];
    std::snprintf(sumbuf, sizeof(sumbuf), "%.9g", h.sum);
    o << name << "_sum{" << labels << "} " << sumbuf << '\n';
    o << name << "_count{" << labels << "} " << h.count << '\n';
}

}  // namespace

std::string EncodePrometheusText(const MetricsSnapshot& snap) {
    const std::string L = CommonLabels(snap);
    std::ostringstream o;

    AppendCounter(o, "readgen_bytes_read_total", "Total bytes read", L, snap.bytes_read_total);
    AppendCounter(o, "readgen_read_ops_total", "Total read/vector-read operations", L,
                  snap.read_ops_total);

    {
        const std::string ok = L + ",result=\"ok\"";
        const std::string fail = L + ",result=\"fail\"";
        o << "# HELP readgen_sessions_total Completed file sessions\n";
        o << "# TYPE readgen_sessions_total counter\n";
        o << "readgen_sessions_total{" << ok << "} " << snap.sessions_ok << '\n';
        o << "readgen_sessions_total{" << fail << "} " << snap.sessions_fail << '\n';
    }

    AppendGauge(o, "readgen_target_rate_bytes", "Configured target rate in bytes/sec", L,
                snap.target_rate_bytes);
    AppendGauge(o, "readgen_achieved_rate_bytes",
                "Cumulative achieved read rate (bytes_read / steady_clock wall sec)", L,
                snap.achieved_rate_bytes);
    AppendHistogram(o, "readgen_open_seconds", "File open latency including redirects", L,
                    snap.open_seconds);
    AppendHistogram(o, "readgen_ttfb_seconds", "Time from open to first byte", L, snap.ttfb_seconds);
    AppendHistogram(o, "readgen_read_seconds", "Per-session read phase duration", L, snap.read_seconds);
    AppendHistogram(o, "readgen_redirects_per_open", "Redirect hop count per open", L,
                    snap.redirects_per_open);

    if (!snap.errors_by_class.empty()) {
        o << "# HELP readgen_errors_total Hard session failures by classifier class\n";
        o << "# TYPE readgen_errors_total counter\n";
        for (const auto& e : snap.errors_by_class) {
            o << "readgen_errors_total{" << L << ",class=\"" << EscapeLabel(e.first) << "\"} "
              << e.second << '\n';
        }
    }

    if (!snap.soft_faults_by_kind.empty()) {
        o << "# HELP readgen_soft_faults_total XrdCl Error-level log lines (may not fail a session)\n";
        o << "# TYPE readgen_soft_faults_total counter\n";
        for (const auto& e : snap.soft_faults_by_kind) {
            o << "readgen_soft_faults_total{" << L << ",kind=\"" << EscapeLabel(e.first) << "\"} "
              << e.second << '\n';
        }
    }

    AppendGaugeU64(o, "readgen_inflight_reads", "Currently in-flight file sessions", L,
                   snap.inflight_reads);
    AppendGaugeU64(o, "readgen_peak_inflight", "Peak in-flight file sessions this run", L,
                   snap.peak_inflight);
    AppendGaugeU64(o, "readgen_workers_configured", "Configured max in-flight workers", L,
                   snap.workers_configured);
    AppendGauge(o, "readgen_cpu_seconds_total", "Process CPU time (utime+stime) in seconds", L,
                snap.cpu_seconds_total);
    AppendGaugeU64(o, "process_resident_memory_bytes", "Process RSS in bytes", L,
                   snap.process_resident_memory_bytes);
    AppendGauge(o, "readgen_wall_seconds", "Elapsed wall time of the run so far", L, snap.wall_s);

    if (!snap.by_data_server.empty()) {
        o << "# HELP readgen_endpoint_bytes_total Bytes read attributed to resolved DataServer\n";
        o << "# TYPE readgen_endpoint_bytes_total counter\n";
        o << "# HELP readgen_endpoint_achieved_rate_bytes Cumulative bytes/wall for this DataServer "
             "(same definition as readgen_achieved_rate_bytes)\n";
        o << "# TYPE readgen_endpoint_achieved_rate_bytes gauge\n";
        o << "# HELP readgen_endpoint_sessions_total Completed FileSessions attributed to resolved "
             "DataServer (not TCP connections)\n";
        o << "# TYPE readgen_endpoint_sessions_total counter\n";
        bool any_ep_errors = false;
        for (const auto& kv : snap.by_data_server) {
            if (!kv.second.errors_by_class.empty()) {
                any_ep_errors = true;
                break;
            }
        }
        if (any_ep_errors) {
            o << "# HELP readgen_endpoint_errors_total Hard failures by DataServer and class\n";
            o << "# TYPE readgen_endpoint_errors_total counter\n";
        }
        for (const auto& kv : snap.by_data_server) {
            const EndpointStats& ep = kv.second;
            std::ostringstream el;
            el << L << ",data_server=\"" << EscapeLabel(ep.data_server) << "\"";
            if (!ep.cms_site.empty()) {
                el << ",cms_site=\"" << EscapeLabel(ep.cms_site) << "\"";
            }
            const std::string EL = el.str();
            const double ep_rate =
                snap.wall_s > 0.0 ? static_cast<double>(ep.bytes_read) / snap.wall_s : 0.0;
            o << "readgen_endpoint_bytes_total{" << EL << "} " << ep.bytes_read << '\n';
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", ep_rate);
                o << "readgen_endpoint_achieved_rate_bytes{" << EL << "} " << buf << '\n';
            }
            o << "readgen_endpoint_sessions_total{" << EL << ",result=\"ok\"} " << ep.sessions_ok
              << '\n';
            o << "readgen_endpoint_sessions_total{" << EL << ",result=\"fail\"} " << ep.sessions_fail
              << '\n';
            for (const auto& err : ep.errors_by_class) {
                o << "readgen_endpoint_errors_total{" << EL << ",class=\"" << EscapeLabel(err.first)
                  << "\"} " << err.second << '\n';
            }
        }
    }

    if (!snap.by_cms_site.empty()) {
        o << "# HELP readgen_site_bytes_total Bytes read attributed to CMS site (mapped servers only)\n";
        o << "# TYPE readgen_site_bytes_total counter\n";
        o << "# HELP readgen_site_achieved_rate_bytes Cumulative bytes/wall for this CMS site\n";
        o << "# TYPE readgen_site_achieved_rate_bytes gauge\n";
        o << "# HELP readgen_site_sessions_total Completed FileSessions attributed to CMS site\n";
        o << "# TYPE readgen_site_sessions_total counter\n";
        for (const auto& kv : snap.by_cms_site) {
            const SiteStats& site = kv.second;
            const std::string SL =
                L + ",cms_site=\"" + EscapeLabel(site.cms_site) + "\"";
            const double site_rate =
                snap.wall_s > 0.0 ? static_cast<double>(site.bytes_read) / snap.wall_s : 0.0;
            o << "readgen_site_bytes_total{" << SL << "} " << site.bytes_read << '\n';
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", site_rate);
                o << "readgen_site_achieved_rate_bytes{" << SL << "} " << buf << '\n';
            }
            o << "readgen_site_sessions_total{" << SL << ",result=\"ok\"} " << site.sessions_ok
              << '\n';
            o << "readgen_site_sessions_total{" << SL << ",result=\"fail\"} " << site.sessions_fail
              << '\n';
        }
    }

    return o.str();
}

}  // namespace readgen
