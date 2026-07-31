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
    std::string target_rate_input;   // original --rate string (for operator echo)
    uint32_t workers = 16;           // max in-flight FileSessions
    PatternType pattern = PatternType::Sequential;
    uint32_t chunk_size = 1 << 20;   // bytes per read chunk
    uint16_t vector_chunks = 8;      // chunks per VectorRead when vector/mixed
    double vector_fraction = 0.4;    // mixed: fraction of sessions that are vector
    double file_fraction = 1.0;      // fraction of each file to read
    uint64_t max_bytes = 0;          // 0 = use file_fraction only (unless max_bytes_auto)
    bool max_bytes_auto = false;     // compute max_bytes once at start from rate / workers
    uint64_t seed = 1;
    bool dry_run = false;

    // Bound a stuck XrdCl session / reconnect (0 = disabled).
    double session_timeout_s = 60.0;
    // XrdCl DefaultEnv overrides applied at run start (seconds / counts).
    int connection_window_s = 15;  // default XrdCl is 120 — too long for a worker slot
    int connection_retry = 2;
    int request_timeout_s = 60;

    // Metrics / FileSink
    std::string results_dir = "results";
    double snapshot_interval_s = 15.0;
    std::string job_id;       // empty → hostname or "local"
    bool write_results = true;  // --no-results disables FileSink
    std::string target = "default";  // label; single-target CLI

    // Pushgateway — empty disables push. Observability stack is external.
    std::string pushgateway_url;
    std::string pushgateway_job = "xrd-readgen";
    bool pushgateway_keep = false;  // if true, skip DELETE on exit (debug / short smoke)

    // Workload reproducibility (empty when launched via legacy CLI flags).
    int schema_version = 0;
    std::string auth_mode;               // e.g. "x509"
    std::string workload_hash;           // SHA-256 of canonical resolved JSON
    std::string workload_resolved_json;  // canonical JSON text written beside results

    // Optional JSON host→CMS-site map (override / fallback for sitename query).
    std::string site_map_path;
    // Query each unique DataServer for `config sitename` (cached). Default on.
    bool sitename_query = true;
};

const char* PatternTypeName(PatternType t);

// ---------------------------------------------------------------------------
// Rate / session sizing policy (compile-time; not CLI-tunable).
//
// --max-bytes auto  (ComputeAutoMaxBytes):
//   charge ≈ (target_rate / workers) × kAutoMaxAmortizeSec
//   floor  = kAutoMaxFloorChunks × chunk_size
//   ceil   = min(target_rate × kRateHeadroomSec, kAutoMaxHardCapBytes)
//
// Pre-Stat token charge  (EstimateSessionCharge):
//   max_bytes if set; else kEstimateChargePartialChunks × chunk when
//   0 < file_fraction < 1; else kEstimateChargeFullChunks × chunk.
//   Vector sessions charge at least one VectorRead op.
//
// Token-bucket burst  (ComputeBucketBurst):
//   max(workers × charge, target_rate × kRateHeadroomSec, charge)
//   Intentional: admits a full worker pipeline immediately, so achieved
//   rate can briefly overshoot --rate at start or after stalls.
//
// End-of-run drain wait  (RunEngine):
//   min(kDrainWaitCapSec, session_timeout_s + kDrainTimeoutGraceSec)
//   when --session-timeout > 0; else kDrainWaitCapSec.
// ---------------------------------------------------------------------------
inline constexpr double kAutoMaxAmortizeSec = 8.0;
inline constexpr uint32_t kAutoMaxFloorChunks = 4;
inline constexpr uint64_t kAutoMaxHardCapBytes = 32ull * 1000 * 1000;  // 32 MB (SI)
inline constexpr uint32_t kEstimateChargePartialChunks = 4;
inline constexpr uint32_t kEstimateChargeFullChunks = 16;
// Seconds of target_rate used as (a) auto max_bytes aggregate cap and
// (b) minimum token-bucket headroom beyond one pipeline of charges.
inline constexpr double kRateHeadroomSec = 2.0;
inline constexpr double kDrainWaitCapSec = 120.0;
inline constexpr double kDrainTimeoutGraceSec = 15.0;

// Per-session byte budget for --max-bytes auto. Requires target_rate_bps > 0.
uint64_t ComputeAutoMaxBytes(const RunConfig& cfg);

// Estimated per-session token charge before Stat knows the real file size.
// Shared by Scheduler (Acquire) and ComputeBucketBurst.
uint64_t EstimateSessionCharge(const RunConfig& cfg, bool use_vector);

// Token-bucket burst capacity from the rate policy above.
uint64_t ComputeBucketBurst(const RunConfig& cfg);

// If max_bytes_auto and rate > 0, fill max_bytes from rate/workers.
// Uncapped (target_rate_bps == 0) requires explicit max_bytes > 0 — throws
// if max_bytes_auto or max_bytes == 0.
void ResolveRunConfig(RunConfig& cfg);

// Join endpoint + path into a root:// URL.
std::string JoinUrl(const std::string& endpoint, const std::string& path);

// Load one path per non-empty, non-# line.
std::vector<std::string> LoadFileList(const std::string& path);

}  // namespace readgen

#endif  // READGEN_RUN_CONFIG_HH
