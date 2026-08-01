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

TEST(Units, RateBitsOnly) {
    EXPECT_EQ(ParseRateString("8Mbps"), 1000000u);     // 8e6 bit/s → 1e6 B/s
    EXPECT_EQ(ParseRateString("1Gbps"), 125000000u);   // 1e9 / 8
    EXPECT_EQ(ParseRateString("280Mbps"), 35000000u);
    EXPECT_EQ(ParseRateString("1000kbps"), 125000u);
    EXPECT_EQ(ParseRateString("8000000"), 1000000u);   // bare = bits/sec
    EXPECT_EQ(ParseRateString("8bps"), 1u);

    // Byte rates removed — must not silently become megabits.
    EXPECT_THROW(ParseRateString("1MBps"), std::runtime_error);
    EXPECT_THROW(ParseRateString("1MiBps"), std::runtime_error);
    EXPECT_THROW(ParseRateString("1MB/s"), std::runtime_error);
    EXPECT_THROW(ParseRateString("35MBps"), std::runtime_error);
}

TEST(Units, TargetRateUncapped) {
    EXPECT_TRUE(IsUncappedRateToken(""));
    EXPECT_TRUE(IsUncappedRateToken("uncapped"));
    EXPECT_TRUE(IsUncappedRateToken("Uncapped"));
    EXPECT_FALSE(IsUncappedRateToken("0"));
    EXPECT_FALSE(IsUncappedRateToken("80Mbps"));
    EXPECT_EQ(ParseTargetRateString(""), 0u);
    EXPECT_EQ(ParseTargetRateString("uncapped"), 0u);
    EXPECT_EQ(ParseTargetRateString("0"), 0u);
    EXPECT_EQ(ParseTargetRateString("0Mbps"), 0u);
    EXPECT_EQ(ParseTargetRateString("80Mbps"), 10000000u);
    EXPECT_THROW(ParseTargetRateString("not-a-rate"), std::runtime_error);
}

TEST(Units, Format) {
    EXPECT_NE(FormatBytes(1000).find("kB"), std::string::npos);
    EXPECT_NE(FormatBytes(1000000).find("MB"), std::string::npos);
    // 35e6 bytes/s → 280 Mbps
    EXPECT_EQ(FormatRate(35000000), "280.00 Mbps");
    EXPECT_EQ(FormatRate(125000000), "1.00 Gbps");
}
