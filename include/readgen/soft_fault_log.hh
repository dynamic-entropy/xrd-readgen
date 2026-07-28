#ifndef READGEN_SOFT_FAULT_LOG_HH
#define READGEN_SOFT_FAULT_LOG_HH

#include "readgen/metrics.hh"

#include <XrdCl/XrdClLog.hh>

#include <string>

namespace readgen {

// Classify an XrdCl Error log line into a coarse soft-fault kind.
std::string ClassifySoftFaultMessage(const std::string& message);

// True if the formatted XrdCl log line is Error-level ([Error  ] / [Error]).
bool IsXrdClErrorLogLine(const std::string& message);

// Tee XrdCl log output to stderr and count Error-level lines as soft faults.
// Does not own the MetricsRegistry; caller must TearDown before registry dies.
class SoftFaultLogOut final : public XrdCl::LogOut {
public:
    explicit SoftFaultLogOut(MetricsRegistry* registry);
    void Write(const std::string& message) override;

    // Install as XrdCl log output (replaces previous). Call TearDown to restore cerr.
    static void Install(MetricsRegistry* registry);
    static void TearDown();

private:
    MetricsRegistry* registry_;
};

}  // namespace readgen

#endif  // READGEN_SOFT_FAULT_LOG_HH
