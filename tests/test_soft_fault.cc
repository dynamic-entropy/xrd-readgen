#include "readgen/soft_fault_log.hh"

#include <gtest/gtest.h>

using readgen::ClassifySoftFaultMessage;
using readgen::IsXrdClErrorLogLine;

TEST(SoftFault, DetectsErrorLines) {
    EXPECT_TRUE(IsXrdClErrorLogLine(
        "[2026-07-28 18:01:39.975255 +0200][Error  ][AsyncSock         ] Socket error"));
    EXPECT_FALSE(IsXrdClErrorLogLine(
        "[2026-07-28 17:59:49.485401 +0200][Info   ][AsyncSock         ] TLS hand-shake done."));
}

TEST(SoftFault, ClassifiesConnectionReset) {
    const char* msg =
        "[Error  ][AsyncSock] Socket error encountered: [ERROR] Socket error: connection reset by "
        "peer";
    EXPECT_EQ(ClassifySoftFaultMessage(msg), "connection");
}

TEST(SoftFault, ClassifiesIoAndTimeout) {
    EXPECT_EQ(ClassifySoftFaultMessage("[Error  ][XRootD] Unable to get the response to request kXR_read"),
              "io");
    EXPECT_EQ(ClassifySoftFaultMessage("[Error  ][PostMaster] operation timeout"), "timeout");
}
