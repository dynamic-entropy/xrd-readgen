#ifndef READGEN_LOG_HH
#define READGEN_LOG_HH

#include <cstdio>
#include <string>

namespace readgen {

// UTC timestamp with milliseconds, e.g. "2026-08-02T10:15:30.123Z".
std::string UtcStamp();

// Print "<stamp> <fmt...>\n" to stream (fmt should omit the trailing newline).
void LogTo(std::FILE* stream, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace readgen

#define READGEN_LOG_ERR(...) ::readgen::LogTo(stderr, __VA_ARGS__)
#define READGEN_LOG_OUT(...) ::readgen::LogTo(stdout, __VA_ARGS__)

#endif  // READGEN_LOG_HH
