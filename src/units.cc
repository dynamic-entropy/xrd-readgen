#include "readgen/units.hh"

#include <cinttypes>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace readgen {
namespace {

bool ParseNumberSuffix(const std::string& s, double& value, std::string& unit) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    size_t start = i;
    bool saw_digit = false;
    bool saw_dot = false;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isdigit(c)) {
            saw_digit = true;
            ++i;
        } else if (c == '.' && !saw_dot) {
            saw_dot = true;
            ++i;
        } else {
            break;
        }
    }
    if (!saw_digit || i == start) return false;
    value = std::stod(s.substr(0, i));
    unit = s.substr(i);
    return true;
}

std::string Lower(std::string u) {
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return u;
}

// SI (KB/MB/GB = 1000^n) vs IEC binary (KiB/MiB/GiB = 1024^n).
uint64_t ScaleBytes(double value, const std::string& unit_in) {
    const std::string u = Lower(unit_in);
    double mul = 1.0;
    if (u.empty() || u == "b" || u == "byte" || u == "bytes")
        mul = 1.0;
    else if (u == "kib")
        mul = 1024.0;
    else if (u == "mib")
        mul = 1024.0 * 1024.0;
    else if (u == "gib")
        mul = 1024.0 * 1024.0 * 1024.0;
    else if (u == "tib")
        mul = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if (u == "k" || u == "kb")
        mul = 1e3;
    else if (u == "m" || u == "mb")
        mul = 1e6;
    else if (u == "g" || u == "gb")
        mul = 1e9;
    else if (u == "t" || u == "tb")
        mul = 1e12;
    else
        throw std::runtime_error("unknown size unit '" + unit_in + "'");
    if (value < 0) throw std::runtime_error("size must be non-negative");
    return static_cast<uint64_t>(value * mul + 0.5);
}

uint64_t BitsPerSecToBytesPerSec(double bits_per_sec) {
    if (bits_per_sec < 0) throw std::runtime_error("rate must be non-negative");
    return static_cast<uint64_t>(bits_per_sec / 8.0 + 0.5);
}

}  // namespace

double ParseDurationString(const std::string& s) {
    double value = 0;
    std::string unit;
    if (!ParseNumberSuffix(s, value, unit)) throw std::runtime_error("cannot parse duration '" + s + "'");
    if (value < 0) throw std::runtime_error("duration must be non-negative");
    const std::string u = Lower(unit);
    if (u.empty() || u == "s" || u == "sec" || u == "secs" || u == "seconds") return value;
    if (u == "m" || u == "min" || u == "mins" || u == "minutes") return value * 60.0;
    if (u == "h" || u == "hr" || u == "hrs" || u == "hour" || u == "hours") return value * 3600.0;
    throw std::runtime_error("unknown duration unit '" + unit + "'");
}

bool IsUncappedRateToken(const std::string& s) {
    if (s.empty()) return true;
    return Lower(s) == "uncapped";
}

uint64_t ParseTargetRateString(const std::string& s) {
    if (IsUncappedRateToken(s)) return 0;
    return ParseRateString(s);
}

uint64_t ParseRateString(const std::string& s) {
    // SI bit rates only → bytes/sec. Reject byte-rate spellings explicitly so
    // "MBps" (megabytes) is not silently folded into "mbps" (megabits).
    std::string t = s;
    if (t.size() >= 2 && (t.compare(t.size() - 2, 2, "/s") == 0 || t.compare(t.size() - 2, 2, "/S") == 0)) {
        throw std::runtime_error(
            "byte rates like '" + s + "' are not accepted; use Mbps/Gbps (SI bits)");
    }
    double value = 0;
    std::string unit;
    if (!ParseNumberSuffix(t, value, unit)) throw std::runtime_error("cannot parse rate '" + s + "'");
    if (value < 0) throw std::runtime_error("rate must be non-negative");

    if (unit.empty()) return BitsPerSecToBytesPerSec(value);

    // Reject byte-rate spellings before case-fold (MBps / MiBps must not become Mbps).
    const std::string u = Lower(unit);
    if (u.find("ibps") != std::string::npos ||
        (unit.size() >= 3 && unit.compare(unit.size() - 3, 3, "Bps") == 0)) {
        throw std::runtime_error(
            "byte rates like '" + s + "' are not accepted; use Mbps/Gbps (SI bits)");
    }
    if (u.size() < 3 || u.compare(u.size() - 3, 3, "bps") != 0) {
        throw std::runtime_error("unknown rate unit '" + unit + "' (use Mbps/Gbps/bps)");
    }
    const std::string prefix = u.substr(0, u.size() - 3);
    double bits = value;
    if (prefix.empty() || prefix == "b") {
        // bps
    } else if (prefix == "k" || prefix == "kb")
        bits *= 1e3;
    else if (prefix == "m" || prefix == "mb")
        bits *= 1e6;
    else if (prefix == "g" || prefix == "gb")
        bits *= 1e9;
    else if (prefix == "t" || prefix == "tb")
        bits *= 1e12;
    else
        throw std::runtime_error("unknown rate unit '" + unit + "' (use Mbps/Gbps/bps)");
    return BitsPerSecToBytesPerSec(bits);
}

uint64_t ParseSizeString(const std::string& s) {
    double value = 0;
    std::string unit;
    if (!ParseNumberSuffix(s, value, unit)) throw std::runtime_error("cannot parse size '" + s + "'");
    return ScaleBytes(value, unit);
}

std::string FormatBytes(uint64_t n) {
    char buf[64];
    const double kb = 1e3;
    const double mb = 1e6;
    const double gb = 1e9;
    if (n >= static_cast<uint64_t>(gb))
        std::snprintf(buf, sizeof(buf), "%.2f GB", n / gb);
    else if (n >= static_cast<uint64_t>(mb))
        std::snprintf(buf, sizeof(buf), "%.2f MB", n / mb);
    else if (n >= static_cast<uint64_t>(kb))
        std::snprintf(buf, sizeof(buf), "%.2f kB", n / kb);
    else
        std::snprintf(buf, sizeof(buf), "%" PRIu64 " B", n);
    return buf;
}

std::string FormatRate(uint64_t bytes_per_sec) {
    const double bits = static_cast<double>(bytes_per_sec) * 8.0;
    char buf[64];
    const double kb = 1e3;
    const double mb = 1e6;
    const double gb = 1e9;
    const double tb = 1e12;
    if (bits >= tb)
        std::snprintf(buf, sizeof(buf), "%.2f Tbps", bits / tb);
    else if (bits >= gb)
        std::snprintf(buf, sizeof(buf), "%.2f Gbps", bits / gb);
    else if (bits >= mb)
        std::snprintf(buf, sizeof(buf), "%.2f Mbps", bits / mb);
    else if (bits >= kb)
        std::snprintf(buf, sizeof(buf), "%.2f kbps", bits / kb);
    else
        std::snprintf(buf, sizeof(buf), "%.0f bps", bits);
    return buf;
}

std::string FormatDuration(double seconds) {
    char buf[64];
    if (seconds >= 3600.0)
        std::snprintf(buf, sizeof(buf), "%.2f h", seconds / 3600.0);
    else if (seconds >= 60.0)
        std::snprintf(buf, sizeof(buf), "%.2f m", seconds / 60.0);
    else
        std::snprintf(buf, sizeof(buf), "%.2f s", seconds);
    return buf;
}

}  // namespace readgen
