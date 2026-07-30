#ifndef READGEN_SITE_MAP_HH
#define READGEN_SITE_MAP_HH

#include <string>
#include <unordered_map>
#include <vector>

namespace readgen {

// Optional hostname → CMS site map. Loaded from a JSON object:
//   {
//     "cmsxrootd.example.org": "T2_XX_SITE",
//     "*.fnal.gov": "T1_US_FNAL",
//     "*.cern.ch": "T2_CH_CERN"
//   }
// Lookup order: exact host:port, exact host, then longest *.domain suffix.
// Never invents a site: miss → empty string (caller keeps hostname only).
class SiteMap {
public:
    SiteMap() = default;

    // Load from JSON file. Throws std::runtime_error on I/O or schema errors.
    static SiteMap LoadFile(const std::string& path);

    // Insert one mapping (host, host:port, or "*.domain" → site).
    void Add(std::string host_or_pattern, std::string cms_site);

    // Returns CMS site name or empty if unknown / unclean.
    std::string Lookup(const std::string& data_server) const;

    size_t size() const { return exact_.size() + suffixes_.size(); }
    bool empty() const { return exact_.empty() && suffixes_.empty(); }

private:
    std::unordered_map<std::string, std::string> exact_;
    // suffix includes leading dot, e.g. ".fnal.gov"; longest match wins.
    std::vector<std::pair<std::string, std::string>> suffixes_;
};

// Strip :port from host:port; leave bracketed IPv6 alone if no simple parse.
std::string HostnameOnly(const std::string& data_server);

}  // namespace readgen

#endif  // READGEN_SITE_MAP_HH
