#ifndef READGEN_FILE_SESSION_HH
#define READGEN_FILE_SESSION_HH

#include <cstdint>
#include <string>

namespace readgen {

// Input for one Open - Stat - Read/VectorRead loop - Close session.
struct FileSessionOptions {
    std::string url;
    uint32_t chunk_size = 1 << 20;  // bytes per read op (default 1 MiB)
    uint64_t offset = 0;            // starting offset
    uint64_t max_bytes = 0;         // 0 = read to EOF
    uint16_t vector_chunks = 0;     // >0: issue VectorReads of N chunks per op
};

// Client-side timings and counters from one completed (or failed) session.
// Timestamps are taken in XrdCl response handlers (steady_clock).
struct FileSessionResult {
    bool ok = false;
    std::string error;  // non-empty when !ok

    std::string url;
    std::string data_server;
    uint64_t file_size = 0;
    size_t open_hosts = 0;

    double open_ms = 0.0;
    double ttfb_ms = 0.0;
    double read_s = 0.0;
    double close_ms = 0.0;
    double total_s = 0.0;

    uint64_t bytes_read = 0;
    uint64_t ops = 0;
    bool vector = false;
    double throughput_mib_s = 0.0;

    double op_lat_min_ms = 0.0;
    double op_lat_avg_ms = 0.0;
    double op_lat_max_ms = 0.0;
};

// Run one async file session; blocks until Close (or failure). Thread-safe
// to call from multiple callers each with their own session (Phase 1 workers).
FileSessionResult RunFileSession(const FileSessionOptions& opts);

}  // namespace readgen

#endif  // READGEN_FILE_SESSION_HH
