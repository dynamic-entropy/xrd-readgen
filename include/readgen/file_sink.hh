#ifndef READGEN_FILE_SINK_HH
#define READGEN_FILE_SINK_HH

#include "readgen/metrics.hh"

#include <cstdint>
#include <fstream>
#include <string>

namespace readgen {

struct RunInfoMeta {
    std::string version;
    std::string arch;
    std::string xrdcl_version;
    uint64_t seed = 0;
    std::string pattern;
    int schema_version = 0;              // 0 = legacy CLI / unknown
    std::string auth_mode;               // e.g. "x509"; empty for legacy CLI
    std::string workload_hash;           // SHA-256 of canonical resolved JSON
    std::string workload_resolved_json;  // written as workload_resolved.json when set
};

// Writes periodic metrics.jsonl snapshots and a final result.json.
class FileSink {
public:
    FileSink(std::string results_dir, std::string run_id, RunInfoMeta meta);

    // Create results_dir/run_id/ and open metrics.jsonl. Throws on failure.
    void Start();

    void WriteSnapshot(const MetricsSnapshot& snap);
    void WriteResult(const MetricsSnapshot& snap, double cpu_seconds_at_start);

    const std::string& run_dir() const { return run_dir_; }

private:
    std::string results_dir_;
    std::string run_id_;
    std::string run_dir_;
    RunInfoMeta meta_;
    std::ofstream jsonl_;
};

}  // namespace readgen

#endif  // READGEN_FILE_SINK_HH
