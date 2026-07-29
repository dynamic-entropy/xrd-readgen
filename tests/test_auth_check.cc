#include "readgen/auth_check.hh"

#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

using readgen::CheckX509Credentials;
using readgen::kCredentialSafetyMarginSec;
namespace fs = std::filesystem;

namespace {

fs::path TempDir() {
    const auto dir = fs::temp_directory_path() / ("readgen_auth_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

bool WriteSelfSignedPem(const fs::path& path, long not_before_offset_s, long lifetime_s) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    RSA* rsa = RSA_new();
    BIGNUM* bn = BN_new();
    if (!pkey || !rsa || !bn) return false;
    BN_set_word(bn, RSA_F4);
    if (RSA_generate_key_ex(rsa, 2048, bn, nullptr) != 1) return false;
    if (EVP_PKEY_assign_RSA(pkey, rsa) != 1) return false;
    rsa = nullptr;  // owned by pkey

    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        BN_free(bn);
        return false;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), not_before_offset_s);
    X509_gmtime_adj(X509_get_notAfter(cert), not_before_offset_s + lifetime_s);
    X509_set_pubkey(cert, pkey);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("readgen-test"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_sign(cert, pkey, EVP_sha256());

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        BN_free(bn);
        return false;
    }
    PEM_write_X509(f, cert);
    std::fclose(f);
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);

    X509_free(cert);
    EVP_PKEY_free(pkey);
    BN_free(bn);
    return true;
}

bool MessagesContainSecret(const readgen::AuthCheckResult& r, const std::string& pem_snippet) {
    for (const auto& i : r.issues) {
        if (i.message.find(pem_snippet) != std::string::npos) return true;
        if (i.message.find("BEGIN CERTIFICATE") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(AuthCheck, MissingProxy) {
    const auto dir = TempDir();
    const auto path = dir / "missing.pem";
    const auto r = CheckX509Credentials(30.0, path.string());
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.issues.empty());
    EXPECT_EQ(r.issues.front().field, "auth.proxy");
    fs::remove_all(dir);
}

TEST(AuthCheck, ValidFutureProxy) {
    const auto dir = TempDir();
    const auto path = dir / "ok.pem";
    ASSERT_TRUE(WriteSelfSignedPem(path, -60, 3600));
    const auto r = CheckX509Credentials(30.0, path.string());
    EXPECT_TRUE(r.ok) << (r.issues.empty() ? "" : r.issues.front().message);
    EXPECT_GT(r.remaining_s, 30.0 + kCredentialSafetyMarginSec - 5.0);
    EXPECT_FALSE(MessagesContainSecret(r, "MII"));
    fs::remove_all(dir);
}

TEST(AuthCheck, ExpiredProxy) {
    const auto dir = TempDir();
    const auto path = dir / "expired.pem";
    ASSERT_TRUE(WriteSelfSignedPem(path, -7200, 3600));  // ended ~1h ago
    const auto r = CheckX509Credentials(30.0, path.string());
    EXPECT_FALSE(r.ok);
    bool found = false;
    for (const auto& i : r.issues) {
        if (i.field == "auth.lifetime") found = true;
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(MessagesContainSecret(r, "BEGIN"));
    fs::remove_all(dir);
}

TEST(AuthCheck, TooShortForWorkload) {
    const auto dir = TempDir();
    const auto path = dir / "short.pem";
    // ~400s remaining; need 600 + 300 = 900
    ASSERT_TRUE(WriteSelfSignedPem(path, -60, 460));
    const auto r = CheckX509Credentials(600.0, path.string());
    EXPECT_FALSE(r.ok);
    bool found = false;
    for (const auto& i : r.issues) {
        if (i.field == "auth.lifetime") found = true;
    }
    EXPECT_TRUE(found);
    fs::remove_all(dir);
}

TEST(AuthCheck, LoosePermissionsFail) {
    const auto dir = TempDir();
    const auto path = dir / "loose.pem";
    ASSERT_TRUE(WriteSelfSignedPem(path, -60, 7200));
    fs::permissions(path, fs::perms::owner_all | fs::perms::group_write);
    const auto r = CheckX509Credentials(30.0, path.string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.loose_permissions);
    fs::remove_all(dir);
}
