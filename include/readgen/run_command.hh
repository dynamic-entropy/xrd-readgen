#ifndef READGEN_RUN_COMMAND_HH
#define READGEN_RUN_COMMAND_HH

#include "readgen/run_config.hh"

namespace readgen {

// Execute a CLI-driven run (or --dry-run). Returns process exit code.
int RunRunCommand(const RunConfig& cfg);

}  // namespace readgen

#endif  // READGEN_RUN_COMMAND_HH
