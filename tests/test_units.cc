#include "readgen/units.hh"

#include <stdexcept>

#include <gtest/gtest.h>

using readgen::FormatBytes;
using readgen::FormatRate;
using readgen::IsUncappedRateToken;
using readgen::ParseDurationString;
using readgen::ParseRateString;
using readgen::ParseSizeString;
using readgen::ParseTargetRateString;

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

TEST(Units, TargetRateUncapped) {
    EXPECT_TRUE(IsUncappedRateToken(""));
    EXPECT_TRUE(IsUncappedRateToken("uncapped"));
    EXPECT_TRUE(IsUncappedRateToken("Uncapped"));
    EXPECT_FALSE(IsUncappedRateToken("0"));
    EXPECT_FALSE(IsUncappedRateToken("10MBps"));
    EXPECT_EQ(ParseTargetRateString(""), 0u);
    EXPECT_EQ(ParseTargetRateString("uncapped"), 0u);
    EXPECT_EQ(ParseTargetRateString("0"), 0u);
    EXPECT_EQ(ParseTargetRateString("0MBps"), 0u);
    EXPECT_EQ(ParseTargetRateString("10MBps"), 10000000u);
    EXPECT_THROW(ParseTargetRateString("not-a-rate"), std::runtime_error);
}

TEST(Units, Format) {
    EXPECT_NE(FormatBytes(1000).find("kB"), std::string::npos);
    EXPECT_NE(FormatBytes(1000000).find("MB"), std::string::npos);
    EXPECT_NE(FormatRate(35000000).find("MB/s"), std::string::npos);
}
