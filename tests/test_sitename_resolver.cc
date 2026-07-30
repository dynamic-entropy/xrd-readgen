#include "readgen/sitename_resolver.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

using readgen::NormalizeSitename;
using readgen::SitenameResolver;

TEST(NormalizeSitename, TrimsWhitespaceAndNuls) {
    EXPECT_EQ(NormalizeSitename("US_CMS_T1_DCACHE_DISK\n"), "US_CMS_T1_DCACHE_DISK");
    EXPECT_EQ(NormalizeSitename("  T2_CH_CERN\r\n"), "T2_CH_CERN");
    EXPECT_EQ(NormalizeSitename(std::string("X\0\0", 3)), "X");
}

TEST(SitenameResolver, CachesSuccessesOnly) {
    std::atomic<int> calls{0};
    SitenameResolver resolver([&](const std::string& ds) {
        ++calls;
        if (ds.find("fnal") != std::string::npos) return std::string("US_CMS_T1_DCACHE_DISK");
        return std::string{};
    });

    EXPECT_EQ(resolver.Resolve("cmsstor204.fnal.gov:21115"), "US_CMS_T1_DCACHE_DISK");
    EXPECT_EQ(resolver.Resolve("cmsstor204.fnal.gov:21115"), "US_CMS_T1_DCACHE_DISK");
    EXPECT_EQ(resolver.Resolve("cmsstor205.fnal.gov:31197"), "US_CMS_T1_DCACHE_DISK");
    // Failures are not cached — each Resolve re-queries.
    EXPECT_EQ(resolver.Resolve("unknown.example:1094"), "");
    EXPECT_EQ(resolver.Resolve("unknown.example:1094"), "");
    EXPECT_EQ(calls.load(), 4);  // 2 unique FNAL hits + 2 miss retries
    EXPECT_EQ(resolver.cache_size(), 2u);
}

TEST(SitenameResolver, SkipsEmptyAndUnknownSentinel) {
    std::atomic<int> calls{0};
    SitenameResolver resolver([&](const std::string&) {
        ++calls;
        return std::string("SHOULD_NOT_RUN");
    });
    EXPECT_EQ(resolver.Resolve(""), "");
    EXPECT_EQ(resolver.Resolve("unknown"), "");
    EXPECT_EQ(calls.load(), 0);
}
