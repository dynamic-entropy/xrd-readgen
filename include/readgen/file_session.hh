#ifndef READGEN_FILE_SESSION_HH
#define READGEN_FILE_SESSION_HH

#include <cstdint>
#include <functional>
#include <string>

namespace readgen {

// Input for one Open → Stat → Read/VectorRead loop → Close session.
struct FileSessionOptions {
    std::string url;
    uint32_t chunk_size = 1 << 20;  // bytes per read op (default 1 MiB)
    uint64_t offset = 0;            // starting offset (ignored if random_offset)
    uint64_t max_bytes = 0;         // 0 = no hard cap beyond file_fraction
    uint16_t vector_chunks = 0;     // >0: issue VectorReads of N chunks per op
    double file_fraction = 1.0;     // fraction of file size to read (after Stat)
    bool random_offset = false;     // pick start offset after Stat (seeded)
    uint64_t offset_seed = 0;       // RNG seed for random_offset
};

// Client-side timings and counters from one completed (or failed) session.
// Timestamps are taken in XrdCl response handlers (steady_clock).
struct FileSessionResult {
    bool ok = false;
    std::string error;  // non-empty when !ok
    int status_code = 0;
    int err_code = 0;

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

using FileSessionDone = std::function<void(FileSessionResult)>;

// Run one async file session; blocks until Close (or failure).
FileSessionResult RunFileSession(const FileSessionOptions& opts);

// Fire-and-forget: submit Open and return immediately. Invokes on_done from an
// XrdCl worker thread when the session finishes. Session is heap-allocated and
// self-deleting. on_done must be safe to call from that thread.
void StartFileSession(const FileSessionOptions& opts, FileSessionDone on_done);

}  // namespace readgen

#endif  // READGEN_FILE_SESSION_HH
