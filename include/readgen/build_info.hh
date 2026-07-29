#ifndef READGEN_BUILD_INFO_HH
#define READGEN_BUILD_INFO_HH

namespace readgen {

inline const char* BuildArch() {
#if defined(__aarch64__) || defined(__arm64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

}  // namespace readgen

#endif  // READGEN_BUILD_INFO_HH
