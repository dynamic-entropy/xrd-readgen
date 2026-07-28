#include "readgen/error_classifier.hh"

#include <gtest/gtest.h>

using readgen::ClassifyXRootDError;
using readgen::ErrorClass;
using readgen::ErrorClassName;

TEST(ErrorClassifier, Auth) {
    EXPECT_EQ(ClassifyXRootDError(0, 3010, ""), ErrorClass::Auth);
    EXPECT_EQ(ClassifyXRootDError(0, 0, "not authorized"), ErrorClass::Auth);
}

TEST(ErrorClassifier, NotFound) {
    EXPECT_EQ(ClassifyXRootDError(0, 3001, ""), ErrorClass::NotFound);
    EXPECT_EQ(ClassifyXRootDError(0, 0, "No such file"), ErrorClass::NotFound);
}

TEST(ErrorClassifier, TimeoutConnection) {
    EXPECT_EQ(ClassifyXRootDError(0, 110, "operation timed out"), ErrorClass::Timeout);
    EXPECT_EQ(ClassifyXRootDError(0, 111, "connection refused"), ErrorClass::Connection);
}

TEST(ErrorClassifier, RedirectLoop) {
    EXPECT_EQ(ClassifyXRootDError(0, 0, "redirect loop detected"), ErrorClass::RedirectLoop);
    EXPECT_EQ(ClassifyXRootDError(0, 0, "[FATAL] Redirect limit has been reached"), ErrorClass::RedirectLoop);
}

TEST(ErrorClassifier, TlsConnection) {
    EXPECT_EQ(ClassifyXRootDError(0, 0, "[FATAL] TLS error: resource temporarily unavailable"),
              ErrorClass::Connection);
    EXPECT_EQ(ClassifyXRootDError(0, 0, "Failed to do TLS connect: error_ssl"), ErrorClass::Connection);
}

TEST(ErrorClassifier, Names) {
    EXPECT_STREQ(ErrorClassName(ErrorClass::Auth), "auth");
    EXPECT_STREQ(ErrorClassName(ErrorClass::None), "none");
    EXPECT_STREQ(ErrorClassName(ErrorClass::RedirectLoop), "redirect_loop");
}
