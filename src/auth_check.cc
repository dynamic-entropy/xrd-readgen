#include "readgen/auth_check.hh"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace readgen {
namespace {

namespace fs = std::filesystem;

void AddIssue(AuthCheckResult& r, std::string field, std::string message) {
    r.issues.push_back(ValidationIssue{std::move(field), std::move(message)});
}

// Seconds from now until notAfter (negative if expired). Uses OpenSSL "now".
double RemainingSeconds(const ASN1_TIME* not_after) {
    if (!not_after) return 0;
    int day = 0;
    int sec = 0;
    if (ASN1_TIME_diff(&day, &sec, nullptr, not_after) != 1) return 0;
    return static_cast<double>(day) * 86400.0 + static_cast<double>(sec);
}

}  // namespace

std::string DefaultX509ProxyPath() {
    if (const char* env = std::getenv("X509_USER_PROXY")) {
        if (env[0] != '\0') return std::string(env);
    }
    return "/tmp/x509up_u" + std::to_string(static_cast<unsigned>(geteuid()));
}

AuthCheckResult CheckX509Credentials(double required_duration_s,
                                     const std::string& proxy_path_override) {
    AuthCheckResult r;
    r.proxy_path = proxy_path_override.empty() ? DefaultX509ProxyPath() : proxy_path_override;

    std::error_code ec;
    const fs::path path(r.proxy_path);
    if (!fs::exists(path, ec)) {
        AddIssue(r, "auth.proxy", "credential file not found (set X509_USER_PROXY or create default proxy)");
        return r;
    }
    if (!fs::is_regular_file(path, ec)) {
        AddIssue(r, "auth.proxy", "credential path is not a regular file");
        return r;
    }

    const auto perms = fs::status(path, ec).permissions();
    if (ec) {
        AddIssue(r, "auth.proxy", "cannot stat credential file");
        return r;
    }
    if ((perms & (fs::perms::group_write | fs::perms::others_write)) != fs::perms::none) {
        r.loose_permissions = true;
        AddIssue(r, "auth.proxy", "credential file is group/other-writable; tighten permissions");
        // Continue — still attempt lifetime parse; treat as hard fail below.
    }

    BIO* bio = BIO_new_file(r.proxy_path.c_str(), "r");
    if (!bio) {
        AddIssue(r, "auth.proxy", "cannot open credential file for reading");
        return r;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) {
        ERR_clear_error();
        AddIssue(r, "auth.proxy", "cannot parse X509 certificate/proxy PEM");
        return r;
    }

    r.remaining_s = RemainingSeconds(X509_get0_notAfter(cert));
    X509_free(cert);
    if (r.remaining_s <= 0) {
        AddIssue(r, "auth.lifetime", "credential expired");
        return r;
    }

    const double need = required_duration_s + kCredentialSafetyMarginSec;
    if (r.remaining_s < need) {
        AddIssue(r, "auth.lifetime",
                 "credential remaining lifetime is shorter than workload duration + " +
                     std::to_string(static_cast<int>(kCredentialSafetyMarginSec)) + "s safety margin");
        return r;
    }

    if (r.loose_permissions) {
        // Permissions issue already recorded — fail closed.
        return r;
    }

    r.ok = true;
    return r;
}

}  // namespace readgen
