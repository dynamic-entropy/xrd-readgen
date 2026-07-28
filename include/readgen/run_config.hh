#ifndef READGEN_RUN_CONFIG_HH
#define READGEN_RUN_CONFIG_HH

#include <cstdint>
#include <string>
#include <vector>

namespace readgen {

enum class PatternType { Sequential, Random, Vector, Mixed };

struct RunConfig {
    std::string run_id = "run";
    double duration_s = 30.0;
    std::string endpoint;  // e.g. root://localhost:10945/
    std::vector<std::string> files;  // paths relative to endpoint (or absolute LFNs)
    std::string filelist_path;       // source path (for dry-run display)
    uint64_t target_rate_bps = 0;    // 0 = uncapped
    uint32_t workers = 4;            // max in-flight FileSessions
    PatternType pattern = PatternType::Sequential;
    uint32_t chunk_size = 1 << 20;   // bytes per read chunk
    uint16_t vector_chunks = 8;      // chunks per VectorRead when vector/mixed
    double vector_fraction = 0.4;    // mixed: fraction of sessions that are vector
    double file_fraction = 1.0;      // fraction of each file to read
    uint64_t max_bytes = 0;          // 0 = use file_fraction only (unless max_bytes_auto)
    bool max_bytes_auto = false;     // compute max_bytes from rate / workers
    bool reopen = true;
    uint64_t seed = 1;
    bool dry_run = false;

    // Metrics / FileSink (Chunk 3)
    std::string results_dir = "results";
    double snapshot_interval_s = 15.0;
    std::string job_id;       // empty → hostname or "local"
    bool write_results = true;  // --no-results disables FileSink
    std::string target = "default";  // label; single-target CLI
};

const char* PatternTypeName(PatternType t);

// Per-session byte budget sized to sustain target_rate with `workers`
// concurrent sessions (amortizes open/TTFB). Requires target_rate_bps > 0.
uint64_t ComputeAutoMaxBytes(const RunConfig& cfg);

// If max_bytes_auto, fill max_bytes. Ensures token-bucket burst can cover a charge.
void ResolveRunConfig(RunConfig& cfg);

// Join endpoint + path into a root:// URL.
std::string JoinUrl(const std::string& endpoint, const std::string& path);

// Load one path per non-empty, non-# line.
std::vector<std::string> LoadFileList(const std::string& path);

}  // namespace readgen

#endif  // READGEN_RUN_CONFIG_HH
