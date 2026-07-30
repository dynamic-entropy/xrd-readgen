#ifndef READGEN_WORKLOAD_RUN_COMMAND_HH
#define READGEN_WORKLOAD_RUN_COMMAND_HH

#include "readgen/run_config.hh"

#include <functional>
#include <string>

namespace readgen {

// Optional injectable engine for unit tests. Empty → RunRunCommand.
using WorkloadEngineRunner = std::function<int(const RunConfig&)>;

struct WorkloadRunOptions {
    std::string workload_path;
    std::string target;  // empty = ok only when the workload has exactly one target
    bool dry_run = false;
    bool skip_auth_check = false;
    std::string site_map_path;  // optional CLI override of sinks.site_map
    bool sitename_query = true;
    WorkloadEngineRunner run_hook;
};

// Validate → select one target → x509 auth preflight → ToRunConfig → engine.
// Exit codes match RunRunCommand (0/1/2); precondition failures are 2.
int RunWorkloadCommand(const WorkloadRunOptions& opts);

}  // namespace readgen

#endif  // READGEN_WORKLOAD_RUN_COMMAND_HH
