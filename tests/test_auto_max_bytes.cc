#include "readgen/run_config.hh"
#include "readgen/token_bucket.hh"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

using readgen::ComputeAutoMaxBytes;
using readgen::ResolveRunConfig;
using readgen::RunConfig;
using readgen::TokenBucket;
using Clock = std::chrono::steady_clock;

TEST(TokenBucket, ChargeLargerThanBurstFailsFast) {
    TokenBucket b(50ull << 20, 50ull << 20);  // 50 MiB/s, 50 MiB burst
    const auto deadline = Clock::now() + std::chrono::milliseconds(50);
    EXPECT_FALSE(b.AcquireUntil(100ull << 20, deadline));  // 100 MiB > burst
}

TEST(AutoMaxBytes, ScalesWithWorkers) {
    RunConfig cfg;
    cfg.target_rate_bps = 50ull << 20;  // 50 MiB/s
    cfg.chunk_size = 1 << 20;
    cfg.workers = 8;
    const uint64_t a = ComputeAutoMaxBytes(cfg);
    cfg.workers = 32;
    const uint64_t b = ComputeAutoMaxBytes(cfg);
    EXPECT_GT(a, b);
    EXPECT_GE(a, 4ull << 20);
    EXPECT_LE(a, cfg.target_rate_bps * 2);
}

TEST(AutoMaxBytes, ResolveSetsMaxBytes) {
    RunConfig cfg;
    cfg.target_rate_bps = 50ull << 20;
    cfg.chunk_size = 1 << 20;
    cfg.workers = 16;
    cfg.max_bytes_auto = true;
    ResolveRunConfig(cfg);
    EXPECT_GT(cfg.max_bytes, 0u);
}

TEST(AutoMaxBytes, RequiresRate) {
    RunConfig cfg;
    cfg.max_bytes_auto = true;
    cfg.target_rate_bps = 0;
    EXPECT_THROW(ResolveRunConfig(cfg), std::runtime_error);
}
