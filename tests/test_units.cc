#include "readgen/units.hh"

#include <gtest/gtest.h>

using readgen::FormatBytes;
using readgen::FormatRate;
using readgen::ParseDurationString;
using readgen::ParseRateString;
using readgen::ParseSizeString;

TEST(Units, Duration) {
    EXPECT_DOUBLE_EQ(ParseDurationString("30"), 30.0);
    EXPECT_DOUBLE_EQ(ParseDurationString("30s"), 30.0);
    EXPECT_DOUBLE_EQ(ParseDurationString("5m"), 300.0);
    EXPECT_DOUBLE_EQ(ParseDurationString("1h"), 3600.0);
    EXPECT_THROW(ParseDurationString("abc"), std::runtime_error);
}

TEST(Units, Size) {
    EXPECT_EQ(ParseSizeString("1024"), 1024u);
    EXPECT_EQ(ParseSizeString("1KiB"), 1024u);
    EXPECT_EQ(ParseSizeString("1MiB"), 1024u * 1024u);
    EXPECT_EQ(ParseSizeString("1MB"), 1000000u);
    EXPECT_EQ(ParseSizeString("1KB"), 1000u);
}

TEST(Units, Rate) {
    EXPECT_EQ(ParseRateString("1MiBps"), 1024u * 1024u);
    EXPECT_EQ(ParseRateString("1MBps"), 1000000u);
    EXPECT_EQ(ParseRateString("1MB/s"), 1000000u);
    EXPECT_EQ(ParseRateString("8Mbps"), 1000000u);   // 8e6 bits/s / 8
    EXPECT_EQ(ParseRateString("1Gbps"), 125000000u);
    EXPECT_EQ(ParseRateString("35MBps"), 35000000u);
}

TEST(Units, Format) {
    EXPECT_NE(FormatBytes(1000).find("kB"), std::string::npos);
    EXPECT_NE(FormatBytes(1000000).find("MB"), std::string::npos);
    EXPECT_NE(FormatRate(35000000).find("MB/s"), std::string::npos);
}
