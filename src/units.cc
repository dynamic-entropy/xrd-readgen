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

uint64_t ScaleBinary(double value, const std::string& unit_in) {
    const std::string u = Lower(unit_in);
    double mul = 1.0;
    if (u.empty() || u == "b" || u == "byte" || u == "bytes")
        mul = 1.0;
    else if (u == "k" || u == "kb" || u == "kib")
        mul = 1024.0;
    else if (u == "m" || u == "mb" || u == "mib")
        mul = 1024.0 * 1024.0;
    else if (u == "g" || u == "gb" || u == "gib")
        mul = 1024.0 * 1024.0 * 1024.0;
    else if (u == "t" || u == "tb" || u == "tib")
        mul = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else
        throw std::runtime_error("unknown size unit '" + unit_in + "'");
    if (value < 0) throw std::runtime_error("size must be non-negative");
    return static_cast<uint64_t>(value * mul + 0.5);
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

uint64_t ParseRateString(const std::string& s) {
    // Accept: 100MiBps, 1Gbps, 500MB/s, 1e6 (raw bytes/sec)
    std::string t = s;
    // Normalize "/s" → trailing handled below
    if (t.size() >= 2 && (t.compare(t.size() - 2, 2, "/s") == 0 || t.compare(t.size() - 2, 2, "/S") == 0)) {
        t.resize(t.size() - 2);
    }
    double value = 0;
    std::string unit;
    if (!ParseNumberSuffix(t, value, unit)) throw std::runtime_error("cannot parse rate '" + s + "'");
    if (value < 0) throw std::runtime_error("rate must be non-negative");

    std::string u = Lower(unit);
    // Strip trailing "ps" or "bps"
    if (u.size() >= 2 && u.compare(u.size() - 2, 2, "ps") == 0) u.resize(u.size() - 2);
    if (u.size() >= 3 && u.compare(u.size() - 3, 3, "bps") == 0) u.resize(u.size() - 3);

    // Bit rates: gbps-style after stripping ps → "gb", but "gib" stays binary bytes.
    // Treat plain g/m/k without 'i' and without 'b' as bits if original had bps.
    const bool looked_like_bits =
        Lower(unit).find("bps") != std::string::npos ||
        (Lower(unit).size() >= 2 && Lower(unit).compare(Lower(unit).size() - 2, 2, "ps") == 0 &&
         Lower(unit).find('i') == std::string::npos);

    if (u.empty()) return static_cast<uint64_t>(value + 0.5);

    if (looked_like_bits && (u == "g" || u == "gb" || u == "m" || u == "mb" || u == "k" || u == "kb")) {
        double bits = value;
        if (u[0] == 'k') bits *= 1e3;
        else if (u[0] == 'm') bits *= 1e6;
        else if (u[0] == 'g') bits *= 1e9;
        return static_cast<uint64_t>(bits / 8.0 + 0.5);
    }

    // Byte rates: MiB, MB, GiB, …
    if (!u.empty() && u.back() == 'b') {
        // already has b
    } else if (u == "k" || u == "m" || u == "g" || u == "t") {
        u.push_back('b');
    }
    return ScaleBinary(value, u);
}

uint64_t ParseSizeString(const std::string& s) {
    double value = 0;
    std::string unit;
    if (!ParseNumberSuffix(s, value, unit)) throw std::runtime_error("cannot parse size '" + s + "'");
    return ScaleBinary(value, unit);
}

std::string FormatBytes(uint64_t n) {
    char buf[64];
    const double kib = 1024.0;
    const double mib = kib * 1024.0;
    const double gib = mib * 1024.0;
    if (n >= static_cast<uint64_t>(gib))
        std::snprintf(buf, sizeof(buf), "%.2f GiB", n / gib);
    else if (n >= static_cast<uint64_t>(mib))
        std::snprintf(buf, sizeof(buf), "%.2f MiB", n / mib);
    else if (n >= static_cast<uint64_t>(kib))
        std::snprintf(buf, sizeof(buf), "%.2f KiB", n / kib);
    else
        std::snprintf(buf, sizeof(buf), "%" PRIu64 " B", n);
    return buf;
}

std::string FormatRate(uint64_t bytes_per_sec) {
    return FormatBytes(bytes_per_sec) + "/s";
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
