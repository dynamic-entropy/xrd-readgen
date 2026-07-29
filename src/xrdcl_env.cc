#include "readgen/xrdcl_env.hh"

#include "readgen/units.hh"

#include <XrdCl/XrdClDefaultEnv.hh>

#include <cstdio>

namespace readgen {

void ApplyXrdClTimeouts(int connection_window_s, int connection_retry, int request_timeout_s,
                        double session_timeout_s, bool log) {
    XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
    if (!env) return;
    if (connection_window_s > 0) env->PutInt("ConnectionWindow", connection_window_s);
    if (connection_retry >= 0) env->PutInt("ConnectionRetry", connection_retry);
    if (request_timeout_s > 0) env->PutInt("RequestTimeout", request_timeout_s);
    if (!log) return;
    std::fprintf(stderr,
                 "xrdcl timeouts: ConnectionWindow=%ds ConnectionRetry=%d RequestTimeout=%ds "
                 "session_timeout=%s\n",
                 connection_window_s, connection_retry, request_timeout_s,
                 session_timeout_s > 0 ? FormatDuration(session_timeout_s).c_str() : "off");
}

}  // namespace readgen
