#include "readgen/token_bucket.hh"

#include <algorithm>
#include <thread>

namespace readgen {

TokenBucket::TokenBucket(uint64_t rate_bps, uint64_t burst_bytes)
    : rate_bps_(rate_bps),
      capacity_(burst_bytes > 0 ? burst_bytes : (rate_bps > 0 ? rate_bps : 1)),
      tokens_(static_cast<double>(capacity_)),
      last_(std::chrono::steady_clock::now()) {}

void TokenBucket::RefillUnlocked(std::chrono::steady_clock::time_point now) {
    if (rate_bps_ == 0) return;
    const double dt = std::chrono::duration<double>(now - last_).count();
    if (dt <= 0) return;
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + dt * static_cast<double>(rate_bps_));
    last_ = now;
}

bool TokenBucket::TryAcquire(uint64_t n) {
    if (rate_bps_ == 0 || n == 0) return true;
    if (n > capacity_) return false;
    std::lock_guard<std::mutex> lock(mu_);
    RefillUnlocked(std::chrono::steady_clock::now());
    if (tokens_ < static_cast<double>(n)) return false;
    tokens_ -= static_cast<double>(n);
    return true;
}

bool TokenBucket::AcquireUntil(uint64_t n, std::chrono::steady_clock::time_point deadline) {
    if (rate_bps_ == 0 || n == 0) return true;
    // Charge larger than burst can never succeed (tokens capped at capacity_).
    if (n > capacity_) return false;

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            RefillUnlocked(std::chrono::steady_clock::now());
            if (tokens_ >= static_cast<double>(n)) {
                tokens_ -= static_cast<double>(n);
                return true;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;

        double deficit = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            RefillUnlocked(now);
            deficit = static_cast<double>(n) - tokens_;
        }
        if (deficit <= 0) continue;

        auto sleep_for = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(deficit / static_cast<double>(rate_bps_)));
        sleep_for = std::min(sleep_for, std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now));
        sleep_for = std::min(sleep_for, std::chrono::nanoseconds(20'000'000));
        if (sleep_for.count() > 0) std::this_thread::sleep_for(sleep_for);
    }
}

void TokenBucket::Refund(uint64_t n) {
    if (rate_bps_ == 0 || n == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    RefillUnlocked(std::chrono::steady_clock::now());
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + static_cast<double>(n));
}

}  // namespace readgen
