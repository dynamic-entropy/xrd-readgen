#ifndef READGEN_ERROR_CLASSIFIER_HH
#define READGEN_ERROR_CLASSIFIER_HH

#include <string>

namespace readgen {

enum class ErrorClass {
    None,
    Auth,
    Timeout,
    Connection,
    ServerError,
    NotFound,
    ClientError,
    RedirectLoop,
    Unknown,
};

const char* ErrorClassName(ErrorClass c);

// Classify from XRootD status/err codes (and optional message for heuristics).
ErrorClass ClassifyXRootDError(int status_code, int err_code, const std::string& message = {});

}  // namespace readgen

#endif  // READGEN_ERROR_CLASSIFIER_HH
