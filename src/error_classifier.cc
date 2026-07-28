#include "readgen/error_classifier.hh"

#include <cctype>
#include <string>

namespace readgen {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const char* ErrorClassName(ErrorClass c) {
    switch (c) {
        case ErrorClass::None:
            return "none";
        case ErrorClass::Auth:
            return "auth";
        case ErrorClass::Timeout:
            return "timeout";
        case ErrorClass::Connection:
            return "connection";
        case ErrorClass::ServerError:
            return "server_error";
        case ErrorClass::NotFound:
            return "not_found";
        case ErrorClass::ClientError:
            return "client_error";
        case ErrorClass::RedirectLoop:
            return "redirect_loop";
        case ErrorClass::Unknown:
            return "unknown";
    }
    return "unknown";
}

ErrorClass ClassifyXRootDError(int status_code, int err_code, const std::string& message) {
    (void)status_code;
    const std::string msg = Lower(message);

    // Auth
    if (err_code == 3010 || msg.find("not authorized") != std::string::npos ||
        msg.find("authentication") != std::string::npos || msg.find("permission denied") != std::string::npos ||
        msg.find("eacces") != std::string::npos || msg.find("gss error") != std::string::npos) {
        return ErrorClass::Auth;
    }

    // Not found
    if (err_code == 3001 || msg.find("not found") != std::string::npos ||
        msg.find("no such file") != std::string::npos) {
        return ErrorClass::NotFound;
    }

    // Redirect exhaustion / loops (AAA often surfaces "Redirect limit has been reached")
    if (msg.find("redirect limit") != std::string::npos || msg.find("redirect loop") != std::string::npos ||
        msg.find("too many redirects") != std::string::npos || msg.find("redirectlimit") != std::string::npos) {
        return ErrorClass::RedirectLoop;
    }

    // Timeouts
    if (msg.find("timeout") != std::string::npos || msg.find("timed out") != std::string::npos ||
        err_code == 110 /* ETIMEDOUT */) {
        return ErrorClass::Timeout;
    }

    // Transport / TLS / socket (FNAL handshakes often look like this)
    if (msg.find("tls") != std::string::npos || msg.find("error_ssl") != std::string::npos ||
        msg.find("ssl") != std::string::npos || msg.find("handshake") != std::string::npos ||
        msg.find("connection") != std::string::npos || msg.find("connect") != std::string::npos ||
        msg.find("socket") != std::string::npos || msg.find("resource temporarily unavailable") != std::string::npos ||
        msg.find("network is unreachable") != std::string::npos || err_code == 111 /* ECONNREFUSED */ ||
        err_code == 104 /* ECONNRESET */ || err_code == 11 /* EAGAIN */) {
        return ErrorClass::Connection;
    }

    // Remaining XRootD server-ish codes (kXR_* in 3000–3999).
    if (err_code >= 3000 && err_code < 4000) {
        if (err_code == 3005 /* kXR_InvalidRequest */ || err_code == 3002 /* kXR_NotFile */)
            return ErrorClass::ClientError;
        return ErrorClass::ServerError;
    }

    if (msg.find("server") != std::string::npos || msg.find("srverr") != std::string::npos) {
        return ErrorClass::ServerError;
    }

    if (!msg.empty() || err_code != 0 || status_code != 0) return ErrorClass::Unknown;
    return ErrorClass::None;
}

}  // namespace readgen
