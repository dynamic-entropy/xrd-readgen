#ifndef READGEN_RANDOM_IO_HH
#define READGEN_RANDOM_IO_HH

#include <cstdint>
#include <random>

namespace readgen {

// Uniform random file offset for a read of `len` bytes.
// Returns 0 when the file cannot hold a full `len` window.
inline uint64_t SampleRandomReadOffset(std::mt19937_64& rng, uint64_t file_size, uint32_t len) {
    if (file_size == 0 || len == 0 || static_cast<uint64_t>(len) >= file_size) {
        return 0;
    }
    return std::uniform_int_distribution<uint64_t>(0, file_size - len)(rng);
}

}  // namespace readgen

#endif  // READGEN_RANDOM_IO_HH
