#include "readgen/workload_spec.hh"

#include "readgen/site_map.hh"
#include "readgen/units.hh"

#include <openssl/evp.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace readgen {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

void AddIssue(ValidateResult& r, std::string field, std::string message) {
    r.issues.push_back(ValidationIssue{std::move(field), std::move(message)});
}

bool IsObject(const json& j) { return j.is_object(); }

void RejectUnknown(ValidateResult& r, const json& obj, const std::set<std::string>& allowed,
                   const std::string& path) {
    if (!obj.is_object()) return;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!allowed.count(it.key())) {
            AddIssue(r, path.empty() ? it.key() : path + "." + it.key(), "unknown field");
        }
    }
}

bool RequireString(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                   std::string& out) {
    if (!parent.contains(key)) {
        AddIssue(r, path, "required field missing");
        return false;
    }
    if (!parent[key].is_string()) {
        AddIssue(r, path, "must be a string");
        return false;
    }
    out = parent[key].get<std::string>();
    return true;
}

bool OptionalString(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                    std::string& out) {
    if (!parent.contains(key)) return true;
    if (!parent[key].is_string()) {
        AddIssue(r, path, "must be a string");
        return false;
    }
    out = parent[key].get<std::string>();
    return true;
}

bool OptionalUint(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                  uint64_t& out) {
    if (!parent.contains(key)) return true;
    if (!parent[key].is_number_integer() || parent[key].get<json::number_integer_t>() < 0) {
        AddIssue(r, path, "must be a non-negative integer");
        return false;
    }
    out = parent[key].get<uint64_t>();
    return true;
}

bool OptionalInt(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                 int& out, int min_v, int max_v) {
    if (!parent.contains(key)) return true;
    if (!parent[key].is_number_integer()) {
        AddIssue(r, path, "must be an integer");
        return false;
    }
    const auto v = parent[key].get<json::number_integer_t>();
    if (v < min_v || v > max_v) {
        AddIssue(r, path, "out of range");
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool OptionalBool(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                  bool& out) {
    if (!parent.contains(key)) return true;
    if (!parent[key].is_boolean()) {
        AddIssue(r, path, "must be a boolean");
        return false;
    }
    out = parent[key].get<bool>();
    return true;
}

bool OptionalDouble(ValidateResult& r, const json& parent, const char* key, const std::string& path,
                    double& out, double min_v, double max_v) {
    if (!parent.contains(key)) return true;
    if (!parent[key].is_number()) {
        AddIssue(r, path, "must be a number");
        return false;
    }
    const double v = parent[key].get<double>();
    if (v < min_v || v > max_v) {
        AddIssue(r, path, "out of range");
        return false;
    }
    out = v;
    return true;
}

PatternType ParsePatternType(ValidateResult& r, const std::string& s, const std::string& path) {
    if (s == "sequential") return PatternType::Sequential;
    if (s == "random") return PatternType::Random;
    if (s == "vector") return PatternType::Vector;
    if (s == "mixed") return PatternType::Mixed;
    AddIssue(r, path, "must be sequential|random|vector|mixed");
    return PatternType::Sequential;
}

std::string ParentDir(const std::string& path) {
    fs::path p(path);
    auto parent = p.parent_path();
    if (parent.empty()) return ".";
    return parent.string();
}

std::string ResolveRelative(const std::string& base_dir, const std::string& rel) {
    fs::path p(rel);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (fs::path(base_dir) / p).lexically_normal().string();
}

std::string Sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 digest failed");
    }
    EVP_MD_CTX_free(ctx);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out[2 * i] = hex[(digest[i] >> 4) & 0xf];
        out[2 * i + 1] = hex[digest[i] & 0xf];
    }
    return out;
}

json PatternToJson(const PatternSpec& p) {
    json j = json::object();
    j["file_fraction"] = p.file_fraction;
    j["max_bytes"] = p.max_bytes_input;
    j["max_bytes_auto"] = p.max_bytes_auto;
    j["max_bytes_resolved"] = p.max_bytes;
    j["read_size"] = p.read_size_input;
    j["read_size_bytes"] = p.chunk_size;
    j["type"] = PatternTypeName(p.type);
    j["vector_chunks"] = p.vector_chunks;
    j["vector_fraction"] = p.vector_fraction;
    return j;
}

json TargetToJson(const TargetSpec& t) {
    // Use the original filelist string (not the resolved absolute path) so the
    // workload hash is stable across hosts and working directories.
    json j = json::object();
    j["endpoint"] = t.endpoint;
    j["file_count"] = t.files.size();
    j["filelist"] = t.filelist_input;
    j["files"] = t.files;
    j["name"] = t.name;
    j["pattern"] = PatternToJson(t.pattern);
    j["target_rate"] = t.target_rate_input;
    j["target_rate_bps"] = t.target_rate_bps;
    j["workers"] = t.workers;
    return j;
}

json WorkloadToCanonicalJson(const WorkloadSpec& wl) {
    json targets = json::array();
    for (const auto& t : wl.targets) targets.push_back(TargetToJson(t));

    json push = json::object();
    push["job"] = wl.sinks.pushgateway.job;
    push["keep"] = wl.sinks.pushgateway.keep;
    push["url"] = wl.sinks.pushgateway.url;

    json sinks = json::object();
    sinks["job_id"] = wl.sinks.job_id;
    sinks["pushgateway"] = push;
    sinks["results_dir"] = wl.sinks.results_dir;
    sinks["site_map"] = wl.sinks.site_map_input;
    sinks["snapshot_interval"] = wl.sinks.snapshot_interval_input;
    sinks["snapshot_interval_s"] = wl.sinks.snapshot_interval_s;
    sinks["write_results"] = wl.sinks.write_results;

    json tuning = json::object();
    tuning["connection_retry"] = wl.client_tuning.connection_retry;
    tuning["connection_window"] = wl.client_tuning.connection_window_s;
    tuning["request_timeout"] = wl.client_tuning.request_timeout_s;
    tuning["session_timeout"] = wl.client_tuning.session_timeout_input;
    tuning["session_timeout_s"] = wl.client_tuning.session_timeout_s;

    json auth = json::object();
    auth["mode"] = wl.auth.mode;

    json j = json::object();
    j["auth"] = auth;
    j["client_tuning"] = tuning;
    j["duration"] = wl.duration_input;
    j["duration_s"] = wl.duration_s;
    j["run_id"] = wl.run_id;
    j["schema_version"] = wl.schema_version;
    j["seed"] = wl.seed;
    j["sinks"] = sinks;
    j["targets"] = targets;
    return j;
}

}  // namespace

ValidateResult ValidateWorkloadJson(const json& root, const std::string& workload_dir) {
    ValidateResult r;
    WorkloadSpec wl;

    if (!IsObject(root)) {
        AddIssue(r, "", "workload root must be a JSON object");
        return r;
    }

    static const std::set<std::string> top_allowed = {
        "schema_version", "run_id", "duration", "seed", "auth", "targets", "client_tuning", "sinks"};
    RejectUnknown(r, root, top_allowed, "");

    // schema_version
    if (!root.contains("schema_version")) {
        AddIssue(r, "schema_version", "required field missing");
    } else if (!root["schema_version"].is_number_integer()) {
        AddIssue(r, "schema_version", "must be an integer");
    } else {
        wl.schema_version = root["schema_version"].get<int>();
        if (wl.schema_version != 1) {
            AddIssue(r, "schema_version", "unsupported schema_version (expected 1)");
        }
    }

    if (!RequireString(r, root, "run_id", "run_id", wl.run_id)) {
        // issue already recorded
    } else if (wl.run_id.empty()) {
        AddIssue(r, "run_id", "must be non-empty");
    }

    if (!RequireString(r, root, "duration", "duration", wl.duration_input)) {
        // issue
    } else {
        try {
            wl.duration_s = ParseDurationString(wl.duration_input);
            if (wl.duration_s <= 0.0) AddIssue(r, "duration", "must be > 0");
        } catch (const std::exception& e) {
            AddIssue(r, "duration", e.what());
        }
    }

    OptionalUint(r, root, "seed", "seed", wl.seed);

    // auth
    if (!root.contains("auth")) {
        AddIssue(r, "auth", "required field missing");
    } else if (!root["auth"].is_object()) {
        AddIssue(r, "auth", "must be an object");
    } else {
        const json& auth = root["auth"];
        RejectUnknown(r, auth, {"mode"}, "auth");
        std::string mode;
        if (RequireString(r, auth, "mode", "auth.mode", mode)) {
            if (mode != "x509") {
                AddIssue(r, "auth.mode",
                         "unsupported auth mode (only \"x509\" / X509_USER_PROXY in v1; "
                         "bearer tokens deferred until after Phases 3–6)");
            } else {
                wl.auth.mode = mode;
            }
        }
    }

    // client_tuning (optional; defaults match RunConfig)
    if (root.contains("client_tuning")) {
        if (!root["client_tuning"].is_object()) {
            AddIssue(r, "client_tuning", "must be an object");
        } else {
            const json& ct = root["client_tuning"];
            RejectUnknown(r, ct,
                          {"session_timeout", "connection_window", "connection_retry",
                           "request_timeout"},
                          "client_tuning");
            if (OptionalString(r, ct, "session_timeout", "client_tuning.session_timeout",
                               wl.client_tuning.session_timeout_input)) {
                if (ct.contains("session_timeout")) {
                    try {
                        wl.client_tuning.session_timeout_s =
                            ParseDurationString(wl.client_tuning.session_timeout_input);
                        if (wl.client_tuning.session_timeout_s < 0.0) {
                            AddIssue(r, "client_tuning.session_timeout", "must be >= 0");
                        }
                    } catch (const std::exception& e) {
                        AddIssue(r, "client_tuning.session_timeout", e.what());
                    }
                }
            }
            OptionalInt(r, ct, "connection_window", "client_tuning.connection_window",
                        wl.client_tuning.connection_window_s, 1, 86400);
            OptionalInt(r, ct, "connection_retry", "client_tuning.connection_retry",
                        wl.client_tuning.connection_retry, 0, 1000);
            OptionalInt(r, ct, "request_timeout", "client_tuning.request_timeout",
                        wl.client_tuning.request_timeout_s, 1, 86400);
        }
    }

    // sinks (optional)
    if (root.contains("sinks")) {
        if (!root["sinks"].is_object()) {
            AddIssue(r, "sinks", "must be an object");
        } else {
            const json& sinks = root["sinks"];
            RejectUnknown(r, sinks,
                          {"results_dir", "snapshot_interval", "job_id", "write_results",
                           "pushgateway", "site_map"},
                          "sinks");
            OptionalString(r, sinks, "results_dir", "sinks.results_dir", wl.sinks.results_dir);
            if (OptionalString(r, sinks, "snapshot_interval", "sinks.snapshot_interval",
                               wl.sinks.snapshot_interval_input)) {
                if (sinks.contains("snapshot_interval")) {
                    try {
                        wl.sinks.snapshot_interval_s =
                            ParseDurationString(wl.sinks.snapshot_interval_input);
                        if (wl.sinks.snapshot_interval_s <= 0.0) {
                            AddIssue(r, "sinks.snapshot_interval", "must be > 0");
                        }
                    } catch (const std::exception& e) {
                        AddIssue(r, "sinks.snapshot_interval", e.what());
                    }
                }
            }
            OptionalString(r, sinks, "job_id", "sinks.job_id", wl.sinks.job_id);
            OptionalBool(r, sinks, "write_results", "sinks.write_results", wl.sinks.write_results);
            if (OptionalString(r, sinks, "site_map", "sinks.site_map", wl.sinks.site_map_input)) {
                if (!wl.sinks.site_map_input.empty()) {
                    wl.sinks.site_map = ResolveRelative(workload_dir, wl.sinks.site_map_input);
                    try {
                        (void)SiteMap::LoadFile(wl.sinks.site_map);
                    } catch (const std::exception& e) {
                        AddIssue(r, "sinks.site_map", e.what());
                    }
                }
            }
            if (sinks.contains("pushgateway")) {
                if (!sinks["pushgateway"].is_object()) {
                    AddIssue(r, "sinks.pushgateway", "must be an object");
                } else {
                    const json& pg = sinks["pushgateway"];
                    RejectUnknown(r, pg, {"url", "job", "keep"}, "sinks.pushgateway");
                    OptionalString(r, pg, "url", "sinks.pushgateway.url", wl.sinks.pushgateway.url);
                    OptionalString(r, pg, "job", "sinks.pushgateway.job", wl.sinks.pushgateway.job);
                    OptionalBool(r, pg, "keep", "sinks.pushgateway.keep", wl.sinks.pushgateway.keep);
                }
            }
        }
    }

    // targets
    if (!root.contains("targets")) {
        AddIssue(r, "targets", "required field missing");
    } else if (!root["targets"].is_array()) {
        AddIssue(r, "targets", "must be an array");
    } else if (root["targets"].empty()) {
        AddIssue(r, "targets", "must contain at least one target");
    } else {
        std::set<std::string> names;
        size_t idx = 0;
        for (const auto& tj : root["targets"]) {
            const std::string base = "targets[" + std::to_string(idx) + "]";
            TargetSpec t;
            if (!tj.is_object()) {
                AddIssue(r, base, "must be an object");
                ++idx;
                continue;
            }
            RejectUnknown(r, tj,
                          {"name", "endpoint", "filelist", "target_rate", "workers", "pattern"},
                          base);

            if (!RequireString(r, tj, "name", base + ".name", t.name) || t.name.empty()) {
                if (tj.contains("name") && tj["name"].is_string() && t.name.empty()) {
                    AddIssue(r, base + ".name", "must be non-empty");
                }
            } else if (!names.insert(t.name).second) {
                AddIssue(r, base + ".name", "duplicate target name");
            }

            if (RequireString(r, tj, "endpoint", base + ".endpoint", t.endpoint)) {
                if (t.endpoint.compare(0, 7, "root://") != 0) {
                    AddIssue(r, base + ".endpoint", "must start with root://");
                }
            }

            if (RequireString(r, tj, "filelist", base + ".filelist", t.filelist_input)) {
                t.filelist = ResolveRelative(workload_dir, t.filelist_input);
                try {
                    t.files = LoadFileList(t.filelist);
                } catch (const std::exception& e) {
                    AddIssue(r, base + ".filelist", e.what());
                }
            }

            if (tj.contains("target_rate")) {
                if (!tj["target_rate"].is_string()) {
                    AddIssue(r, base + ".target_rate", "must be a string");
                } else {
                    t.target_rate_input = tj["target_rate"].get<std::string>();
                    if (!t.target_rate_input.empty()) {
                        try {
                            t.target_rate_bps = ParseRateString(t.target_rate_input);
                            if (t.target_rate_bps == 0) {
                                AddIssue(r, base + ".target_rate", "must be > 0 when set");
                            }
                        } catch (const std::exception& e) {
                            AddIssue(r, base + ".target_rate", e.what());
                        }
                    }
                }
            }

            if (tj.contains("workers")) {
                if (!tj["workers"].is_number_integer() ||
                    tj["workers"].get<json::number_integer_t>() < 1 ||
                    tj["workers"].get<json::number_integer_t>() > 100000) {
                    AddIssue(r, base + ".workers", "must be an integer in [1, 100000]");
                } else {
                    t.workers = static_cast<uint32_t>(tj["workers"].get<uint64_t>());
                }
            }

            if (!tj.contains("pattern")) {
                AddIssue(r, base + ".pattern", "required field missing");
            } else if (!tj["pattern"].is_object()) {
                AddIssue(r, base + ".pattern", "must be an object");
            } else {
                const json& pj = tj["pattern"];
                RejectUnknown(r, pj,
                              {"type", "read_size", "file_fraction", "vector_fraction",
                               "vector_chunks", "max_bytes"},
                              base + ".pattern");
                std::string type_s;
                if (RequireString(r, pj, "type", base + ".pattern.type", type_s)) {
                    t.pattern.type = ParsePatternType(r, type_s, base + ".pattern.type");
                }
                if (OptionalString(r, pj, "read_size", base + ".pattern.read_size",
                                   t.pattern.read_size_input)) {
                    if (pj.contains("read_size")) {
                        try {
                            const uint64_t sz = ParseSizeString(t.pattern.read_size_input);
                            if (sz == 0 || sz > 0xffffffffu) {
                                AddIssue(r, base + ".pattern.read_size", "out of range");
                            } else {
                                t.pattern.chunk_size = static_cast<uint32_t>(sz);
                            }
                        } catch (const std::exception& e) {
                            AddIssue(r, base + ".pattern.read_size", e.what());
                        }
                    }
                }
                OptionalDouble(r, pj, "file_fraction", base + ".pattern.file_fraction",
                               t.pattern.file_fraction, 0.0, 1.0);
                OptionalDouble(r, pj, "vector_fraction", base + ".pattern.vector_fraction",
                               t.pattern.vector_fraction, 0.0, 1.0);
                if (pj.contains("vector_chunks")) {
                    if (!pj["vector_chunks"].is_number_integer() ||
                        pj["vector_chunks"].get<json::number_integer_t>() < 1 ||
                        pj["vector_chunks"].get<json::number_integer_t>() > 65535) {
                        AddIssue(r, base + ".pattern.vector_chunks",
                                 "must be an integer in [1, 65535]");
                    } else {
                        t.pattern.vector_chunks =
                            static_cast<uint16_t>(pj["vector_chunks"].get<uint64_t>());
                    }
                }
                if (pj.contains("max_bytes")) {
                    if (!pj["max_bytes"].is_string()) {
                        AddIssue(r, base + ".pattern.max_bytes", "must be a string");
                    } else {
                        t.pattern.max_bytes_input = pj["max_bytes"].get<std::string>();
                        if (t.pattern.max_bytes_input == "auto") {
                            t.pattern.max_bytes_auto = true;
                            t.pattern.max_bytes = 0;
                        } else {
                            t.pattern.max_bytes_auto = false;
                            try {
                                t.pattern.max_bytes = ParseSizeString(t.pattern.max_bytes_input);
                            } catch (const std::exception& e) {
                                AddIssue(r, base + ".pattern.max_bytes", e.what());
                            }
                        }
                    }
                }
                if (t.pattern.max_bytes_auto && t.target_rate_bps == 0 &&
                    tj.contains("target_rate") &&
                    tj["target_rate"].is_string() &&
                    tj["target_rate"].get<std::string>().empty()) {
                    // uncapped + auto is fine; ResolveRunConfig clears it
                }
            }

            wl.targets.push_back(std::move(t));
            ++idx;
        }
    }

    if (!r.issues.empty()) return r;

    r.resolved = std::move(wl);
    r.canonical_json = WorkloadToCanonicalJson(r.resolved).dump(2) + "\n";
    r.workload_hash = Sha256Hex(r.canonical_json);
    r.ok = true;
    return r;
}

ValidateResult ValidateWorkloadFile(const std::string& path) {
    ValidateResult r;
    std::ifstream in(path);
    if (!in) {
        AddIssue(r, path, "cannot open workload file");
        return r;
    }
    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        AddIssue(r, path, std::string("JSON parse error: ") + e.what());
        return r;
    }
    return ValidateWorkloadJson(root, ParentDir(path));
}

RunConfig ToRunConfig(const WorkloadSpec& wl, const TargetSpec& target) {
    RunConfig cfg;
    cfg.run_id = wl.run_id;
    cfg.duration_s = wl.duration_s;
    cfg.endpoint = target.endpoint;
    cfg.files = target.files;
    cfg.filelist_path = target.filelist;
    cfg.target_rate_bps = target.target_rate_bps;
    cfg.target_rate_input = target.target_rate_input;
    cfg.workers = target.workers;
    cfg.pattern = target.pattern.type;
    cfg.chunk_size = target.pattern.chunk_size;
    cfg.vector_chunks = target.pattern.vector_chunks;
    cfg.vector_fraction = target.pattern.vector_fraction;
    cfg.file_fraction = target.pattern.file_fraction;
    cfg.max_bytes = target.pattern.max_bytes;
    cfg.max_bytes_auto = target.pattern.max_bytes_auto;
    cfg.seed = wl.seed;
    cfg.session_timeout_s = wl.client_tuning.session_timeout_s;
    cfg.connection_window_s = wl.client_tuning.connection_window_s;
    cfg.connection_retry = wl.client_tuning.connection_retry;
    cfg.request_timeout_s = wl.client_tuning.request_timeout_s;
    cfg.results_dir = wl.sinks.results_dir;
    cfg.snapshot_interval_s = wl.sinks.snapshot_interval_s;
    cfg.job_id = wl.sinks.job_id;
    cfg.write_results = wl.sinks.write_results;
    cfg.target = target.name;
    cfg.pushgateway_url = wl.sinks.pushgateway.url;
    cfg.pushgateway_job = wl.sinks.pushgateway.job;
    cfg.pushgateway_keep = wl.sinks.pushgateway.keep;
    cfg.site_map_path = wl.sinks.site_map;
    cfg.schema_version = wl.schema_version;
    cfg.auth_mode = wl.auth.mode;
    return cfg;
}

}  // namespace readgen
