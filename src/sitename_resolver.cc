#include "readgen/sitename_resolver.hh"

#include <XrdCl/XrdClBuffer.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cctype>

namespace readgen {

std::string NormalizeSitename(std::string raw) {
    while (!raw.empty() && (raw.back() == '\0' || std::isspace(static_cast<unsigned char>(raw.back())))) {
        raw.pop_back();
    }
    size_t start = 0;
    while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start]))) ++start;
    if (start > 0) raw.erase(0, start);
    return raw;
}

std::string QueryXrdClSitename(const std::string& data_server, time_t timeout_s) {
    if (data_server.empty() || data_server == "unknown") return {};

    const std::string url = "root://" + data_server + "/";
    XrdCl::FileSystem fs(url);

    XrdCl::Buffer arg;
    arg.FromString("sitename");
    XrdCl::Buffer* response = nullptr;
    const XrdCl::XRootDStatus st =
        fs.Query(XrdCl::QueryCode::Config, arg, response, timeout_s);
    if (!st.IsOK() || response == nullptr) {
        delete response;
        return {};
    }
    std::string out = NormalizeSitename(response->ToString());
    delete response;
    // Servers sometimes echo the key or return placeholders.
    if (out.empty() || out == "sitename" || out == "..." || out == "n/a" || out == "N/A") {
        return {};
    }
    return out;
}

SitenameResolver::SitenameResolver(QueryFn query, time_t timeout_s)
    : query_(std::move(query)), timeout_s_(timeout_s > 0 ? timeout_s : 5) {
    if (!query_) {
        query_ = [t = timeout_s_](const std::string& ds) { return QueryXrdClSitename(ds, t); };
    }
}

std::string SitenameResolver::Resolve(const std::string& data_server) {
    if (data_server.empty() || data_server == "unknown") return {};

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(data_server);
        if (it != cache_.end()) return it->second;
    }

    std::string site = query_ ? query_(data_server) : std::string{};
    // Do not cache failures — a transient Query miss must be retried next pass.
    if (site.empty()) return {};

    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = cache_.emplace(data_server, site);
    if (!inserted) return it->second;  // another thread won the race
    return it->second;
}

size_t SitenameResolver::cache_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.size();
}

}  // namespace readgen
