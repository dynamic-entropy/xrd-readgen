#include "readgen/file_sink.hh"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace readgen {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

json HistogramToJson(const HistogramSnapshot& h) {
    json j;
    j["bounds"] = h.bounds;
    j["counts"] = h.counts;
    j["count"] = h.count;
    j["sum"] = h.sum;
    return j;
}

json LatencyPercentiles(const HistogramSnapshot& h) {
    return json{{"p50", HistogramPercentile(h, 0.50)},
                {"p95", HistogramPercentile(h, 0.95)},
                {"p99", HistogramPercentile(h, 0.99)},
                {"count", h.count},
                {"sum", h.sum}};
}

json SnapshotToJsonl(const MetricsSnapshot& s) {
    json j;
    j["run_id"] = s.run_id;
    j["job_id"] = s.job_id;
    j["target"] = s.target;
    j["endpoint"] = s.endpoint;
    j["wall_s"] = s.wall_s;
    j["readgen_bytes_read_total"] = s.bytes_read_total;
    j["readgen_sessions_total"] = {{"ok", s.sessions_ok}, {"fail", s.sessions_fail}};
    j["readgen_read_ops_total"] = s.read_ops_total;
    j["readgen_target_rate_bytes"] = s.target_rate_bytes;
    j["readgen_achieved_rate_bytes"] = s.achieved_rate_bytes;
    j["readgen_open_seconds"] = HistogramToJson(s.open_seconds);
    j["readgen_ttfb_seconds"] = HistogramToJson(s.ttfb_seconds);
    j["readgen_read_seconds"] = HistogramToJson(s.read_seconds);
    j["readgen_redirects_per_open"] = HistogramToJson(s.redirects_per_open);
    j["readgen_errors_total"] = s.errors_by_class;
    j["readgen_soft_faults_total"] = s.soft_faults_by_kind;
    j["readgen_inflight_reads"] = s.inflight_reads;
    j["readgen_peak_inflight"] = s.peak_inflight;
    j["readgen_workers_configured"] = s.workers_configured;
    j["readgen_cpu_seconds_total"] = s.cpu_seconds_total;
    j["process_resident_memory_bytes"] = s.process_resident_memory_bytes;
    if (!s.by_data_server.empty()) {
        json by_ds = json::object();
        for (const auto& kv : s.by_data_server) {
            const EndpointStats& ep = kv.second;
            json entry = {{"bytes_read", ep.bytes_read},
                          {"sessions_ok", ep.sessions_ok},
                          {"sessions_fail", ep.sessions_fail},
                          {"errors", ep.errors_by_class}};
            if (!ep.cms_site.empty()) entry["cms_site"] = ep.cms_site;
            by_ds[ep.data_server] = std::move(entry);
        }
        j["by_data_server"] = std::move(by_ds);
    }
    if (!s.by_cms_site.empty()) {
        json by_site = json::object();
        for (const auto& kv : s.by_cms_site) {
            const SiteStats& site = kv.second;
            by_site[site.cms_site] = {{"bytes_read", site.bytes_read},
                                      {"sessions_ok", site.sessions_ok},
                                      {"sessions_fail", site.sessions_fail},
                                      {"errors", site.errors_by_class}};
        }
        j["by_cms_site"] = std::move(by_site);
    }
    return j;
}

}  // namespace

FileSink::FileSink(std::string results_dir, std::string run_id, RunInfoMeta meta)
    : results_dir_(std::move(results_dir)),
      run_id_(std::move(run_id)),
      meta_(std::move(meta)) {
    run_dir_ = (fs::path(results_dir_) / run_id_).string();
}

void FileSink::Start() {
    std::error_code ec;
    fs::create_directories(run_dir_, ec);
    if (ec) {
        throw std::runtime_error("cannot create results dir " + run_dir_ + ": " + ec.message());
    }
    if (!meta_.workload_resolved_json.empty()) {
        const auto resolved_path = fs::path(run_dir_) / "workload_resolved.json";
        std::ofstream resolved(resolved_path);
        if (!resolved) {
            throw std::runtime_error("cannot write " + resolved_path.string());
        }
        resolved << meta_.workload_resolved_json;
        if (!meta_.workload_resolved_json.empty() && meta_.workload_resolved_json.back() != '\n') {
            resolved << '\n';
        }
    }
    if (!meta_.workload_hash.empty()) {
        const auto hash_path = fs::path(run_dir_) / "workload.hash";
        std::ofstream hash_out(hash_path);
        if (!hash_out) {
            throw std::runtime_error("cannot write " + hash_path.string());
        }
        hash_out << meta_.workload_hash << '\n';
    }
    const auto path = fs::path(run_dir_) / "metrics.jsonl";
    jsonl_.open(path, std::ios::out | std::ios::trunc);
    if (!jsonl_) {
        throw std::runtime_error("cannot open " + path.string());
    }
}

void FileSink::WriteSnapshot(const MetricsSnapshot& snap) {
    if (!jsonl_) return;
    jsonl_ << SnapshotToJsonl(snap).dump() << '\n';
    jsonl_.flush();
}

void FileSink::WriteResult(const MetricsSnapshot& snap, double cpu_seconds_at_start) {
    const double elapsed = snap.wall_s;
    const double achieved_bps =
        elapsed > 0.0 ? static_cast<double>(snap.bytes_read_total) / elapsed : 0.0;
    const double cpu_delta = snap.cpu_seconds_total - cpu_seconds_at_start;
    const double bytes_per_cpu =
        cpu_delta > 0.0 ? static_cast<double>(snap.bytes_read_total) / cpu_delta : 0.0;

    json j;
    j["run_id"] = snap.run_id;
    j["job_id"] = snap.job_id;
    j["target"] = snap.target;
    j["endpoint"] = snap.endpoint;
    j["elapsed_s"] = elapsed;
    j["target_rate_bps"] = snap.target_rate_bytes;
    j["achieved_bps"] = achieved_bps;
    j["bytes_read"] = snap.bytes_read_total;
    j["sessions_ok"] = snap.sessions_ok;
    j["sessions_fail"] = snap.sessions_fail;
    j["ops"] = snap.read_ops_total;
    j["peak_inflight"] = snap.peak_inflight;
    j["workers"] = snap.workers_configured;
    j["errors"] = snap.errors_by_class;
    j["soft_faults"] = snap.soft_faults_by_kind;
    j["latency"] = {{"open_seconds", LatencyPercentiles(snap.open_seconds)},
                    {"ttfb_seconds", LatencyPercentiles(snap.ttfb_seconds)},
                    {"read_seconds", LatencyPercentiles(snap.read_seconds)},
                    {"redirects_per_open", LatencyPercentiles(snap.redirects_per_open)}};
    j["readgen_cpu_seconds_total"] = snap.cpu_seconds_total;
    j["process_resident_memory_bytes"] = snap.process_resident_memory_bytes;
    j["readgen_bytes_per_cpu_second"] = bytes_per_cpu;
    j["run_info"] = {{"version", meta_.version},
                     {"arch", meta_.arch},
                     {"xrdcl_version", meta_.xrdcl_version},
                     {"seed", meta_.seed},
                     {"pattern", meta_.pattern}};
    if (meta_.schema_version > 0) j["run_info"]["schema_version"] = meta_.schema_version;
    if (!meta_.auth_mode.empty()) j["run_info"]["auth_mode"] = meta_.auth_mode;
    if (!meta_.workload_hash.empty()) j["workload_hash"] = meta_.workload_hash;

    if (!snap.by_data_server.empty()) {
        json by_ds = json::object();
        for (const auto& kv : snap.by_data_server) {
            const EndpointStats& ep = kv.second;
            json entry = {{"bytes_read", ep.bytes_read},
                          {"sessions_ok", ep.sessions_ok},
                          {"sessions_fail", ep.sessions_fail},
                          {"errors", ep.errors_by_class}};
            if (!ep.cms_site.empty()) entry["cms_site"] = ep.cms_site;
            by_ds[ep.data_server] = std::move(entry);
        }
        j["by_data_server"] = std::move(by_ds);
    }
    if (!snap.by_cms_site.empty()) {
        json by_site = json::object();
        for (const auto& kv : snap.by_cms_site) {
            const SiteStats& site = kv.second;
            by_site[site.cms_site] = {{"bytes_read", site.bytes_read},
                                      {"sessions_ok", site.sessions_ok},
                                      {"sessions_fail", site.sessions_fail},
                                      {"errors", site.errors_by_class}};
        }
        j["by_cms_site"] = std::move(by_site);
    }

    const auto path = fs::path(run_dir_) / "result.json";
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << j.dump(2) << '\n';
}

}  // namespace readgen
