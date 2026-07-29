#include "readgen/token_bucket.hh"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using readgen::TokenBucket;
using Clock = std::chrono::steady_clock;

TEST(TokenBucket, Uncapped) {
    TokenBucket b(0);
    EXPECT_TRUE(b.TryAcquire(1ull << 40));
}

TEST(TokenBucket, AcquireAndRefund) {
    TokenBucket b(1 << 20, 1 << 20);  // 1 MiB/s, burst 1 MiB
    EXPECT_TRUE(b.TryAcquire(512 * 1024));
    EXPECT_TRUE(b.TryAcquire(512 * 1024));
    EXPECT_FALSE(b.TryAcquire(1));
    b.Refund(100);
    EXPECT_TRUE(b.TryAcquire(100));
}

TEST(TokenBucket, Refills) {
    TokenBucket b(10 * 1024 * 1024, 1024);  // fast refill, small burst
    EXPECT_TRUE(b.TryAcquire(1024));
    EXPECT_FALSE(b.TryAcquire(1024));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(b.TryAcquire(1024));
}

TEST(TokenBucket, AcquireUntilDeadline) {
    TokenBucket b(1, 1);  // 1 byte/s
    EXPECT_TRUE(b.TryAcquire(1));
    const auto deadline = Clock::now() + std::chrono::milliseconds(5);
    EXPECT_FALSE(b.AcquireUntil(1000000, deadline));
}

TEST(TokenBucket, ChargeLargerThanBurstFailsFast) {
    TokenBucket b(50ull << 20, 50ull << 20);  // 50 MiB/s, 50 MiB burst
    const auto deadline = Clock::now() + std::chrono::milliseconds(50);
    EXPECT_FALSE(b.AcquireUntil(100ull << 20, deadline));  // 100 MiB > burst
}
