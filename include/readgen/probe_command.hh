#ifndef READGEN_PROBE_COMMAND_HH
#define READGEN_PROBE_COMMAND_HH

#include "readgen/file_session.hh"

#include <cstdint>
#include <functional>
#include <string>

namespace readgen {

// Optional injectable session starter for unit tests. Default: StartFileSession.
using ProbeSessionRunner = std::function<void(const FileSessionOptions&, FileSessionDone)>;

struct ProbeOptions {
    std::string workload_path;
    std::string target;           // empty = all targets
    uint32_t limit = 0;           // 0 = all files per target
    uint32_t concurrency = 4;
    bool json = false;
    bool skip_auth_check = false;
    ProbeSessionRunner session_runner;  // empty → StartFileSession
};

// Validate → auth preflight → bounded Open+1-byte probes.
// Exit: 0 all ok, 1 probe failures, 2 schema/auth precondition.
int RunProbeCommand(const ProbeOptions& opts);

}  // namespace readgen

#endif  // READGEN_PROBE_COMMAND_HH
