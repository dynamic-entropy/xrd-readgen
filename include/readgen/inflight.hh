#ifndef READGEN_INFLIGHT_HH
#define READGEN_INFLIGHT_HH

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace readgen {

// Caps concurrent FileSessions. Not an OS thread pool.
class InFlightSemaphore {
   public:
    explicit InFlightSemaphore(uint32_t max_inflight);

    // Block until a slot is free or deadline passes. Returns false on timeout.
    bool AcquireUntil(std::chrono::steady_clock::time_point deadline);

    void Release();

    uint32_t current() const;
    uint32_t peak() const;
    uint32_t max() const { return max_; }

   private:
    const uint32_t max_;
    uint32_t current_ = 0;
    uint32_t peak_ = 0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace readgen

#endif  // READGEN_INFLIGHT_HH
