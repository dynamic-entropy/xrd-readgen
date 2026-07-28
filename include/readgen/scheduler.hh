#ifndef READGEN_SCHEDULER_HH
#define READGEN_SCHEDULER_HH

#include "readgen/file_session.hh"
#include "readgen/run_config.hh"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace readgen {

struct WorkItem {
    FileSessionOptions session;
    uint64_t charge_bytes = 0;  // tokens to acquire before start
};

// Seeded work generator: picks file + session options from RunConfig.
class Scheduler {
   public:
    explicit Scheduler(const RunConfig& cfg);

    WorkItem Next();

   private:
    const RunConfig& cfg_;
    std::mt19937_64 rng_;
    uint64_t seq_ = 0;
};

}  // namespace readgen

#endif  // READGEN_SCHEDULER_HH
