#ifndef READGEN_READ_COMMAND_HH
#define READGEN_READ_COMMAND_HH

#include <cstdint>
#include <string>

namespace readgen {

struct ReadOptions {
  std::string url;
  uint32_t chunk_size = 1 << 20;  // bytes per read op (default 1 MiB)
  uint64_t offset = 0;            // starting offset
  uint64_t max_bytes = 0;         // 0 = read to EOF
  uint16_t vector_chunks = 0;     // >0: issue VectorReads of N chunks per op
  bool json = false;              // machine-readable output
};

// Timed single-file remote read: open -> (stat) -> read loop -> close,
// fully async on XrdCl response handlers. Returns process exit code.
int RunReadCommand(const ReadOptions& opts);

}  // namespace readgen

#endif  // READGEN_READ_COMMAND_HH
