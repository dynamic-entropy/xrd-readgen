#ifndef READGEN_UNITS_HH
#define READGEN_UNITS_HH

#include <cstdint>
#include <string>

namespace readgen {

// Parse human-readable quantities. Throws std::runtime_error on failure.
// Duration: "30", "30s", "5m", "1h" → seconds
double ParseDurationString(const std::string& s);

// Rate: "100MiBps", "1Gbps", "500MB/s" → bytes/sec
uint64_t ParseRateString(const std::string& s);

// Size: "1MiB", "512KiB", "1024" → bytes
uint64_t ParseSizeString(const std::string& s);

std::string FormatBytes(uint64_t n);
std::string FormatRate(uint64_t bytes_per_sec);
std::string FormatDuration(double seconds);

}  // namespace readgen

#endif  // READGEN_UNITS_HH
