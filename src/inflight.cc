#include "readgen/inflight.hh"

#include <algorithm>

namespace readgen {

InFlightSemaphore::InFlightSemaphore(uint32_t max_inflight) : max_(max_inflight == 0 ? 1 : max_inflight) {}

bool InFlightSemaphore::AcquireUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!cv_.wait_until(lock, deadline, [&] { return current_ < max_; })) return false;
    ++current_;
    peak_ = std::max(peak_, current_);
    return true;
}

void InFlightSemaphore::Release() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (current_ > 0) --current_;
    }
    cv_.notify_one();
}

uint32_t InFlightSemaphore::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
}

uint32_t InFlightSemaphore::peak() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peak_;
}

}  // namespace readgen
