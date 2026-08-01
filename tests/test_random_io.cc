#include "readgen/random_io.hh"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

using readgen::SampleRandomReadOffset;

TEST(RandomIo, OffsetZeroWhenLenCoversFile) {
    std::mt19937_64 rng(1);
    EXPECT_EQ(SampleRandomReadOffset(rng, 0, 1024), 0u);
    EXPECT_EQ(SampleRandomReadOffset(rng, 512, 1024), 0u);
    EXPECT_EQ(SampleRandomReadOffset(rng, 1024, 1024), 0u);
}

TEST(RandomIo, OffsetsStayInRange) {
    std::mt19937_64 rng(42);
    constexpr uint64_t kFile = 1ull << 30;
    constexpr uint32_t kLen = 4u << 20;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t off = SampleRandomReadOffset(rng, kFile, kLen);
        EXPECT_LE(off + kLen, kFile);
    }
}

TEST(RandomIo, NotContiguousAscendingRun) {
    // True random I/O must not look like sequential walks of +chunk each step.
    std::mt19937_64 rng(7);
    constexpr uint64_t kFile = 256ull << 20;
    constexpr uint32_t kLen = 1u << 20;
    constexpr int kOps = 32;
    uint64_t prev = 0;
    int ascending_by_chunk = 0;
    std::set<uint64_t> unique;
    for (int i = 0; i < kOps; ++i) {
        const uint64_t off = SampleRandomReadOffset(rng, kFile, kLen);
        unique.insert(off);
        if (i > 0 && off == prev + kLen) {
            ++ascending_by_chunk;
        }
        prev = off;
    }
    EXPECT_LT(ascending_by_chunk, kOps / 4);
    EXPECT_GT(unique.size(), static_cast<size_t>(kOps / 2));
}
