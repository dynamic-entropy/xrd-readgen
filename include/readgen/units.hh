#ifndef READGEN_UNITS_HH
#define READGEN_UNITS_HH

#include <cstdint>
#include <string>

namespace readgen {

// Parse human-readable quantities. Throws std::runtime_error on failure.
// Duration: "30", "30s", "5m", "1h" → seconds
double ParseDurationString(const std::string& s);

// Rate: "100MBps", "100MiBps", "1Gbps", "500MB/s" → bytes/sec
//   MBps / MB/s = SI megabytes/sec; Mbps = megabits/sec; MiBps = mebibytes/sec
uint64_t ParseRateString(const std::string& s);

// True for empty or case-insensitive "uncapped" (capacity mode, no rate hold).
bool IsUncappedRateToken(const std::string& s);

// Like ParseRateString, but empty / "uncapped" / "0" / "0MBps" → 0 (uncapped).
uint64_t ParseTargetRateString(const std::string& s);

// Size: "1MB", "1MiB", "512KiB", "1024" → bytes (MB=SI, MiB=binary)
uint64_t ParseSizeString(const std::string& s);

std::string FormatBytes(uint64_t n);   // SI: kB / MB / GB
std::string FormatRate(uint64_t bytes_per_sec);  // e.g. "28.28 MB/s"
std::string FormatDuration(double seconds);

}  // namespace readgen

#endif  // READGEN_UNITS_HH
