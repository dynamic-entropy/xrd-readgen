#ifndef READGEN_SITENAME_RESOLVER_HH
#define READGEN_SITENAME_RESOLVER_HH

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace readgen {

// Trim whitespace / trailing NULs from an xrdfs/XrdCl sitename response.
std::string NormalizeSitename(std::string raw);

// Query XRootD `query config sitename` for root://host:port/ (empty on failure).
std::string QueryXrdClSitename(const std::string& data_server, time_t timeout_s = 5);

// Cached sitename resolver. Successful answers are cached per data_server;
// failures are not (so a later pass can retry).
class SitenameResolver {
public:
    using QueryFn = std::function<std::string(const std::string& data_server)>;

    explicit SitenameResolver(QueryFn query = {}, time_t timeout_s = 5);

    // Returns advertised sitename or empty if unknown / query failed.
    std::string Resolve(const std::string& data_server);

    size_t cache_size() const;

private:
    QueryFn query_;
    time_t timeout_s_;
    mutable std::mutex mu_;
    // Successful sitenames only (no negative cache).
    std::unordered_map<std::string, std::string> cache_;
};

}  // namespace readgen

#endif  // READGEN_SITENAME_RESOLVER_HH
