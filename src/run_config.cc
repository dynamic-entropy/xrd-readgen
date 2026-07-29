#include "readgen/run_config.hh"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace readgen {

const char* PatternTypeName(PatternType t) {
    switch (t) {
        case PatternType::Sequential:
            return "sequential";
        case PatternType::Random:
            return "random";
        case PatternType::Vector:
            return "vector";
        case PatternType::Mixed:
            return "mixed";
    }
    return "unknown";
}

uint64_t ComputeAutoMaxBytes(const RunConfig& cfg) {
    if (cfg.target_rate_bps == 0) {
        throw std::runtime_error("--max-bytes auto requires --rate");
    }
    const uint32_t w = std::max(cfg.workers, 1u);
    const uint64_t chunk = std::max<uint64_t>(cfg.chunk_size, 1);

    uint64_t bytes = static_cast<uint64_t>((static_cast<double>(cfg.target_rate_bps) /
                                            static_cast<double>(w)) *
                                           kAutoMaxAmortizeSec);

    const uint64_t floor_b = chunk * kAutoMaxFloorChunks;
    const uint64_t aggregate_cap =
        static_cast<uint64_t>(static_cast<double>(cfg.target_rate_bps) * kRateHeadroomSec);
    const uint64_t ceil_b = std::min(aggregate_cap, kAutoMaxHardCapBytes);
    bytes = std::max(bytes, floor_b);
    bytes = std::min(bytes, ceil_b);
    // Align to chunk; if rounding up would breach the cap, round down instead.
    uint64_t aligned = ((bytes + chunk - 1) / chunk) * chunk;
    if (aligned > ceil_b) aligned = (ceil_b / chunk) * chunk;
    return std::max(aligned, chunk);
}

uint64_t EstimateSessionCharge(const RunConfig& cfg, bool use_vector) {
    uint64_t charge;
    if (cfg.max_bytes > 0) {
        charge = cfg.max_bytes;
    } else if (cfg.file_fraction > 0.0 && cfg.file_fraction < 1.0) {
        // Unknown file size: stand-in until Stat; refund/adjust on done.
        charge = static_cast<uint64_t>(cfg.chunk_size) * kEstimateChargePartialChunks;
    } else {
        charge = static_cast<uint64_t>(cfg.chunk_size) * kEstimateChargeFullChunks;
    }
    if (use_vector) {
        charge = std::max(charge, static_cast<uint64_t>(cfg.chunk_size) *
                                      std::max<uint16_t>(1, cfg.vector_chunks));
    }
    return charge;
}

uint64_t ComputeBucketBurst(const RunConfig& cfg) {
    if (cfg.target_rate_bps == 0) return 0;
    const bool may_vector =
        cfg.pattern == PatternType::Vector || cfg.pattern == PatternType::Mixed;
    const uint64_t charge = EstimateSessionCharge(cfg, may_vector);
    const uint64_t pipeline = charge * std::max(cfg.workers, 1u);
    const uint64_t headroom =
        static_cast<uint64_t>(static_cast<double>(cfg.target_rate_bps) * kRateHeadroomSec);
    return std::max({pipeline, headroom, charge});
}

void ResolveRunConfig(RunConfig& cfg) {
    if (cfg.max_bytes_auto) {
        if (cfg.target_rate_bps == 0) {
            cfg.max_bytes_auto = false;
            cfg.max_bytes = 0;
        } else {
            cfg.max_bytes = ComputeAutoMaxBytes(cfg);
        }
    }
}

std::string JoinUrl(const std::string& endpoint, const std::string& path) {
    if (path.empty()) return endpoint;
    // Absolute root:// URL in the filelist — use as-is.
    if (path.compare(0, 7, "root://") == 0) return path;

    std::string ep = endpoint;
    if (ep.empty()) return path;
    // Ensure exactly one trailing slash on the authority so an absolute path
    // ("/lfs/...") becomes root://host:port//lfs/... (XRootD absolute form).
    if (ep.back() != '/') ep.push_back('/');
    if (!path.empty() && path.front() == '/') return ep + path;
    return ep + path;
}

std::vector<std::string> LoadFileList(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open filelist: " + path);

    std::vector<std::string> files;
    std::string line;
    while (std::getline(in, line)) {
        // Trim
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t' || line[a] == '\r')) ++a;
        size_t b = line.size();
        while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == '\r')) --b;
        if (a >= b) continue;
        if (line[a] == '#') continue;
        files.emplace_back(line.substr(a, b - a));
    }
    if (files.empty()) throw std::runtime_error("filelist is empty: " + path);
    return files;
}

}  // namespace readgen
