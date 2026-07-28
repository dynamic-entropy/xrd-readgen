#ifndef READGEN_TOKEN_BUCKET_HH
#define READGEN_TOKEN_BUCKET_HH

#include <chrono>
#include <cstdint>
#include <mutex>

namespace readgen {

// Bytes/sec token bucket. Thread-safe. rate_bps == 0 → uncapped.
class TokenBucket {
   public:
    explicit TokenBucket(uint64_t rate_bps, uint64_t burst_bytes = 0);

    // Non-blocking take. Returns true if n tokens were taken.
    bool TryAcquire(uint64_t n);

    // Block until n tokens available or deadline (steady_clock) passes.
    bool AcquireUntil(uint64_t n, std::chrono::steady_clock::time_point deadline);

    void Refund(uint64_t n);

    uint64_t rate_bps() const { return rate_bps_; }

   private:
    void RefillUnlocked(std::chrono::steady_clock::time_point now);

    const uint64_t rate_bps_;
    const uint64_t capacity_;
    double tokens_ = 0.0;
    std::chrono::steady_clock::time_point last_;
    mutable std::mutex mu_;
};

}  // namespace readgen

#endif  // READGEN_TOKEN_BUCKET_HH
