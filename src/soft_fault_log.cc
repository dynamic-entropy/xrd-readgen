#include "readgen/soft_fault_log.hh"

#include <XrdCl/XrdClDefaultEnv.hh>

#include <cstdio>
#include <mutex>

namespace readgen {
namespace {

std::mutex g_stderr_mu;

}  // namespace

std::string ClassifySoftFaultMessage(const std::string& message) {
    auto has = [&](const char* s) { return message.find(s) != std::string::npos; };

    if (has("timeout") || has("Timeout") || has("TIMEDOUT") || has("etimedout")) return "timeout";
    if (has("redirect") || has("Redirect")) return "redirect";
    if (has("TLS") || has("certificate") || has("Certificate") || has("handshake") ||
        has("authentication") || has("Unauthorized"))
        return "tls_auth";
    if (has("connection reset") || has("Connection refused") || has("Unable to connect") ||
        has("Socket error") || has("Broken pipe") || has("Network is unreachable") ||
        has("No route to host"))
        return "connection";
    if (has("Unable to get the response") || has("kXR_read") || has("kXR_readv") || has("kXR_open"))
        return "io";
    return "other";
}

bool IsXrdClErrorLogLine(const std::string& message) {
    // XrdCl format: ...[Error  ][Topic ...] ...
    return message.find("[Error") != std::string::npos;
}

SoftFaultLogOut::SoftFaultLogOut(MetricsRegistry* registry) : registry_(registry) {}

void SoftFaultLogOut::Write(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_stderr_mu);
        std::fputs(message.c_str(), stderr);
        if (!message.empty() && message.back() != '\n') std::fputc('\n', stderr);
    }
    if (registry_ && IsXrdClErrorLogLine(message)) {
        registry_->ObserveSoftFault(ClassifySoftFaultMessage(message));
    }
}

void SoftFaultLogOut::Install(MetricsRegistry* registry) {
    XrdCl::DefaultEnv::GetLog()->SetOutput(new SoftFaultLogOut(registry));
}

void SoftFaultLogOut::TearDown() {
    XrdCl::DefaultEnv::GetLog()->SetOutput(new XrdCl::LogOutCerr());
}

}  // namespace readgen
