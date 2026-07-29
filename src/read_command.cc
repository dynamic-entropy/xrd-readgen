#include "readgen/read_command.hh"

#include "readgen/file_session.hh"

#include <cinttypes>
#include <cstdio>

namespace readgen {
namespace {

void PrintResult(const FileSessionResult& r, bool json) {
    const char* op = r.vector ? "vector_read" : "read";

    if (json) {
        std::printf(
            "{\"url\":\"%s\",\"data_server\":\"%s\",\"file_size\":%" PRIu64
            ",\"open_hosts\":%zu,\"open_ms\":%.3f,\"ttfb_ms\":%.3f"
            ",\"bytes_read\":%" PRIu64 ",\"ops\":%" PRIu64
            ",\"op\":\"%s\",\"read_s\":%.3f,\"throughput_mb_s\":%.2f"
            ",\"op_latency_ms\":{\"min\":%.3f,\"avg\":%.3f,\"max\":%.3f}"
            ",\"close_ms\":%.3f,\"total_s\":%.3f}\n",
            r.url.c_str(), r.data_server.c_str(), r.file_size, r.open_hosts, r.open_ms, r.ttfb_ms, r.bytes_read, r.ops,
            op, r.read_s, r.throughput_mb_s, r.op_lat_min_ms, r.op_lat_avg_ms, r.op_lat_max_ms, r.close_ms, r.total_s);
        return;
    }

    std::printf("URL:          %s\n", r.url.c_str());
    std::printf("Data server:  %s\n", r.data_server.c_str());
    std::printf("File size:    %" PRIu64 " bytes\n", r.file_size);
    std::printf("Open:         %.3f ms (%zu host%s in open chain)\n", r.open_ms, r.open_hosts,
                r.open_hosts == 1 ? "" : "s");
    std::printf("TTFB:         %.3f ms\n", r.ttfb_ms);
    std::printf("Read:         %" PRIu64 " bytes in %" PRIu64 " %s op%s (%.3f s)\n", r.bytes_read, r.ops, op,
                r.ops == 1 ? "" : "s", r.read_s);
    std::printf("Op latency:   min %.3f / avg %.3f / max %.3f ms\n", r.op_lat_min_ms, r.op_lat_avg_ms, r.op_lat_max_ms);
    std::printf("Throughput:   %.2f MB/s\n", r.throughput_mb_s);
    std::printf("Close:        %.3f ms\n", r.close_ms);
    std::printf("Total:        %.3f s\n", r.total_s);
}

}  // namespace

int RunReadCommand(const ReadOptions& opts) {
    FileSessionOptions session_opts;
    session_opts.url = opts.url;
    session_opts.chunk_size = opts.chunk_size;
    session_opts.offset = opts.offset;
    session_opts.max_bytes = opts.max_bytes;
    session_opts.vector_chunks = opts.vector_chunks;

    const FileSessionResult result = RunFileSession(session_opts);
    if (!result.ok) {
        std::fprintf(stderr, "error: %s\n", result.error.c_str());
        return 1;
    }
    PrintResult(result, opts.json);
    return 0;
}

}  // namespace readgen
