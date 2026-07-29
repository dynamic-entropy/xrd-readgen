#include "readgen/scheduler.hh"

#include <algorithm>

namespace readgen {

Scheduler::Scheduler(const RunConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {}

WorkItem Scheduler::Next() {
    WorkItem item;
    const size_t idx = std::uniform_int_distribution<size_t>(0, cfg_.files.size() - 1)(rng_);
    const std::string& path = cfg_.files[idx];

    item.session.url = JoinUrl(cfg_.endpoint, path);
    item.session.chunk_size = cfg_.chunk_size;
    item.session.file_fraction = cfg_.file_fraction;
    item.session.max_bytes = cfg_.max_bytes;
    item.session.offset = 0;
    item.session.random_offset = false;
    item.session.offset_seed = cfg_.seed ^ (++seq_ * 0x9e3779b97f4a7c15ULL);
    item.session.wall_timeout_s = cfg_.session_timeout_s;

    bool use_vector = false;
    switch (cfg_.pattern) {
        case PatternType::Sequential:
            break;
        case PatternType::Random:
            item.session.random_offset = true;
            break;
        case PatternType::Vector:
            use_vector = true;
            break;
        case PatternType::Mixed: {
            item.session.random_offset =
                std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < 0.5;
            use_vector = std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < cfg_.vector_fraction;
            break;
        }
    }

    if (use_vector) {
        item.session.vector_chunks = std::max<uint16_t>(1, cfg_.vector_chunks);
    }

    item.charge_bytes = EstimateSessionCharge(cfg_, use_vector);
    return item;
}

}  // namespace readgen
