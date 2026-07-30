#include "readgen/site_map.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace readgen {
namespace {

using json = nlohmann::json;

std::string NormalizeKey(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

}  // namespace

std::string HostnameOnly(const std::string& data_server) {
    if (data_server.empty()) return {};
    if (data_server.front() == '[') {
        const auto end = data_server.find(']');
        if (end != std::string::npos) return data_server.substr(0, end + 1);
        return data_server;
    }
    const auto colon = data_server.rfind(':');
    if (colon == std::string::npos) return data_server;
    if (data_server.find(':') != colon) return data_server;
    return data_server.substr(0, colon);
}

void SiteMap::Add(std::string host_or_pattern, std::string cms_site) {
    if (host_or_pattern.empty() || cms_site.empty()) return;
    std::string key = NormalizeKey(std::move(host_or_pattern));
    if (key.size() >= 2 && key[0] == '*' && key[1] == '.') {
        // "*.fnal.gov" → suffix ".fnal.gov"
        const std::string suffix = key.substr(1);
        for (auto& e : suffixes_) {
            if (e.first == suffix) {
                e.second = cms_site;
                return;
            }
        }
        suffixes_.emplace_back(suffix, std::move(cms_site));
        std::sort(suffixes_.begin(), suffixes_.end(),
                  [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        return;
    }
    exact_[std::move(key)] = std::move(cms_site);
}

std::string SiteMap::Lookup(const std::string& data_server) const {
    if (data_server.empty() || empty()) return {};

    const std::string full = NormalizeKey(data_server);
    auto it = exact_.find(full);
    if (it != exact_.end()) return it->second;

    const std::string host = NormalizeKey(HostnameOnly(data_server));
    if (!host.empty()) {
        it = exact_.find(host);
        if (it != exact_.end()) return it->second;

        for (const auto& rule : suffixes_) {
            const std::string& suffix = rule.first;
            if (host.size() > suffix.size() &&
                host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return rule.second;
            }
        }
    }
    return {};
}

SiteMap SiteMap::LoadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open site map: " + path);
    }
    json root;
    try {
        in >> root;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("site map JSON parse error: ") + e.what());
    }
    if (!root.is_object()) {
        throw std::runtime_error("site map: root must be a JSON object of host→cms_site");
    }
    SiteMap map;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_string()) {
            throw std::runtime_error("site map: value for '" + it.key() + "' must be a string");
        }
        map.Add(it.key(), it.value().get<std::string>());
    }
    return map;
}

}  // namespace readgen
