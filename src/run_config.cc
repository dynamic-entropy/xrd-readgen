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

    // ~8s of this worker's fair share of the target rate. Amortizes federation
    // open/TTFB while keeping a single charge within a few seconds of refill.
    constexpr double kAmortizeSec = 8.0;
    uint64_t bytes = static_cast<uint64_t>((static_cast<double>(cfg.target_rate_bps) /
                                            static_cast<double>(w)) *
                                           kAmortizeSec);

    const uint64_t floor_b = chunk * 4;
    // Cap at 2s of aggregate target so one charge cannot starve all workers.
    const uint64_t ceil_b = cfg.target_rate_bps * 2;
    bytes = std::max(bytes, floor_b);
    bytes = std::min(bytes, ceil_b);
    bytes = ((bytes + chunk - 1) / chunk) * chunk;
    return std::max(bytes, chunk);
}

void ResolveRunConfig(RunConfig& cfg) {
    if (cfg.max_bytes_auto) {
        cfg.max_bytes = ComputeAutoMaxBytes(cfg);
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
