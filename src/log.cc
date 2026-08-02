#include "readgen/log.hh"

#include <chrono>
#include <cstdarg>
#include <ctime>
#include <mutex>

namespace readgen {
namespace {

std::mutex g_log_mu;

}  // namespace

std::string UtcStamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                    1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[64];
    const std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    if (n == 0 || n + 5 >= sizeof(buf)) return "1970-01-01T00:00:00.000Z";
    std::snprintf(buf + n, sizeof(buf) - n, ".%03dZ", static_cast<int>(ms.count()));
    return buf;
}

void LogTo(std::FILE* stream, const char* fmt, ...) {
    if (!stream || !fmt) return;
    std::lock_guard<std::mutex> lock(g_log_mu);
    std::fprintf(stream, "%s ", UtcStamp().c_str());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stream, fmt, ap);
    va_end(ap);
    std::fputc('\n', stream);
    std::fflush(stream);
}

}  // namespace readgen
