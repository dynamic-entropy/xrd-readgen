#include "readgen/run_config.hh"

#include <algorithm>
#include <stdexcept>

#include <gtest/gtest.h>

using readgen::ComputeAutoMaxBytes;
using readgen::ComputeBucketBurst;
using readgen::ResolveRunConfig;
using readgen::RunConfig;

TEST(AutoMaxBytes, ScalesWithMaxInflight) {
    RunConfig cfg;
    cfg.target_rate_bps = 50ull << 20;  // 50 MiB/s
    cfg.chunk_size = 1 << 20;
    cfg.max_inflight = 8;
    const uint64_t a = ComputeAutoMaxBytes(cfg);
    cfg.max_inflight = 32;
    const uint64_t b = ComputeAutoMaxBytes(cfg);
    EXPECT_GT(a, b);
    EXPECT_GE(a, static_cast<uint64_t>(cfg.chunk_size) * readgen::kAutoMaxFloorChunks);
    const uint64_t aggregate_cap = static_cast<uint64_t>(
        static_cast<double>(cfg.target_rate_bps) * readgen::kRateHeadroomSec);
    EXPECT_LE(a, std::min(aggregate_cap, readgen::kAutoMaxHardCapBytes));
}

TEST(AutoMaxBytes, RespectsHardCap) {
    RunConfig cfg;
    cfg.target_rate_bps = 500ull * 1000 * 1000;  // 500 MB/s SI
    cfg.chunk_size = 1 << 20;
    cfg.max_inflight = 1;  // amortize would be huge without the hard cap
    EXPECT_LE(ComputeAutoMaxBytes(cfg), readgen::kAutoMaxHardCapBytes);
}

TEST(AutoMaxBytes, ResolveSetsMaxBytes) {
    RunConfig cfg;
    cfg.target_rate_bps = 50ull << 20;
    cfg.chunk_size = 1 << 20;
    cfg.max_inflight = 16;
    cfg.max_bytes_auto = true;
    ResolveRunConfig(cfg);
    EXPECT_GT(cfg.max_bytes, 0u);
    EXPECT_EQ(cfg.max_bytes, ComputeAutoMaxBytes(cfg));
}

TEST(AutoMaxBytes, UncappedRejectsAuto) {
    RunConfig cfg;
    cfg.max_bytes_auto = true;
    cfg.target_rate_bps = 0;
    EXPECT_THROW(ResolveRunConfig(cfg), std::runtime_error);
}

TEST(AutoMaxBytes, UncappedRejectsZeroMaxBytes) {
    RunConfig cfg;
    cfg.max_bytes_auto = false;
    cfg.max_bytes = 0;
    cfg.target_rate_bps = 0;
    EXPECT_THROW(ResolveRunConfig(cfg), std::runtime_error);
}

TEST(AutoMaxBytes, UncappedKeepsExplicitMaxBytes) {
    RunConfig cfg;
    cfg.max_bytes_auto = false;
    cfg.max_bytes = 32ull << 20;
    cfg.target_rate_bps = 0;
    ResolveRunConfig(cfg);
    EXPECT_EQ(cfg.max_bytes, 32ull << 20);
}

TEST(BucketBurst, CoversMaxInflightPipeline) {
    RunConfig cfg;
    cfg.target_rate_bps = 10ull << 20;
    cfg.max_inflight = 4;
    cfg.max_bytes = 20ull << 20;
    cfg.chunk_size = 1 << 20;
    const uint64_t burst = ComputeBucketBurst(cfg);
    EXPECT_GE(burst, cfg.max_bytes * cfg.max_inflight);
    EXPECT_GE(burst, static_cast<uint64_t>(static_cast<double>(cfg.target_rate_bps) *
                                           readgen::kRateHeadroomSec));
}
