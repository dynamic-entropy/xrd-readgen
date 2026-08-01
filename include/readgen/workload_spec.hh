#ifndef READGEN_WORKLOAD_SPEC_HH
#define READGEN_WORKLOAD_SPEC_HH

#include "readgen/run_config.hh"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace readgen {

struct AuthSpec {
    std::string mode = "x509";
};

struct PatternSpec {
    PatternType type = PatternType::Sequential;
    std::string read_size_input = "1MiB";
    uint32_t chunk_size = 1 << 20;
    double vector_fraction = 0.4;
    uint16_t vector_chunks = 8;
    std::string max_bytes_input = "auto";
    uint64_t max_bytes = 0;
    bool max_bytes_auto = true;
};

struct TargetSpec {
    std::string name;
    std::string endpoint;
    std::string filelist;             // resolved path used for LoadFileList
    std::string filelist_input;       // original JSON string
    std::vector<std::string> files;
    std::string target_rate_input;    // empty = uncapped
    uint64_t target_rate_bps = 0;
    uint32_t max_inflight = 16;
    PatternSpec pattern;
};

struct ClientTuningSpec {
    std::string session_timeout_input = "60s";
    double session_timeout_s = 60.0;
    int connection_window_s = 15;
    int connection_retry = 2;
    int request_timeout_s = 60;
};

struct PushgatewaySpec {
    std::string url;
    std::string job = "xrd-readgen";
    bool keep = false;
};

struct SinkSpec {
    std::string results_dir = "results";
    std::string snapshot_interval_input = "15s";
    double snapshot_interval_s = 15.0;
    std::string job_id;
    bool write_results = true;
    PushgatewaySpec pushgateway;
    std::string site_map;        // resolved path (empty = none)
    std::string site_map_input;  // original JSON string
};

struct WorkloadSpec {
    int schema_version = 1;
    std::string run_id = "run";
    std::string duration_input = "30s";
    double duration_s = 30.0;
    uint64_t seed = 1;
    AuthSpec auth;
    std::vector<TargetSpec> targets;
    ClientTuningSpec client_tuning;
    SinkSpec sinks;
};

struct ValidationIssue {
    std::string field;
    std::string message;
};

struct ValidateResult {
    bool ok = false;
    std::vector<ValidationIssue> issues;
    WorkloadSpec resolved;
    std::string canonical_json;
    std::string workload_hash;
};

// Validate a workload JSON document. filelists resolve relative to workload_dir.
// Performs no XRootD I/O.
ValidateResult ValidateWorkloadJson(const nlohmann::json& j, const std::string& workload_dir);

// Read + parse + validate a workload file. Relative filelists resolve against the
// workload file's parent directory.
ValidateResult ValidateWorkloadFile(const std::string& path);

// Map one target into the existing engine RunConfig (used by workload-driven run).
RunConfig ToRunConfig(const WorkloadSpec& wl, const TargetSpec& target);

}  // namespace readgen

#endif  // READGEN_WORKLOAD_SPEC_HH
