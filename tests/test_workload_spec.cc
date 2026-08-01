#include "readgen/workload_spec.hh"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using readgen::ToRunConfig;
using readgen::ValidateWorkloadFile;
using readgen::ValidateWorkloadJson;
using readgen::ValidateResult;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path FixtureDir() {
    // tests/fixtures/workloads relative to repo root or build cwd.
    const fs::path candidates[] = {
        fs::path("tests/fixtures/workloads"),
        fs::path("../tests/fixtures/workloads"),
        fs::path("../../tests/fixtures/workloads"),
    };
    for (const auto& p : candidates) {
        if (fs::exists(p / "valid_minimal.json")) return fs::absolute(p);
    }
    return fs::absolute("tests/fixtures/workloads");
}

bool HasField(const ValidateResult& r, const std::string& field) {
    for (const auto& i : r.issues) {
        if (i.field == field) return true;
    }
    return false;
}

}  // namespace

TEST(WorkloadSpec, ValidMinimalStableHash) {
    const auto path = FixtureDir() / "valid_minimal.json";
    ASSERT_TRUE(fs::exists(path)) << path;
    const auto a = ValidateWorkloadFile(path.string());
    const auto b = ValidateWorkloadFile(path.string());
    ASSERT_TRUE(a.ok) << (a.issues.empty() ? "" : a.issues.front().message);
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(a.workload_hash, b.workload_hash);
    EXPECT_EQ(a.canonical_json, b.canonical_json);
    EXPECT_EQ(a.resolved.schema_version, 1);
    EXPECT_EQ(a.resolved.run_id, "fixture-valid");
    EXPECT_EQ(a.resolved.seed, 42u);
    ASSERT_EQ(a.resolved.targets.size(), 1u);
    EXPECT_EQ(a.resolved.targets[0].files.size(), 1u);
    EXPECT_EQ(a.resolved.targets[0].target_rate_bps, 10000000u);
}

TEST(WorkloadSpec, RelativeFilelistIgnoresCwd) {
    const auto fixture = FixtureDir();
    const auto path = fixture / "valid_minimal.json";
    const fs::path tmp = fs::temp_directory_path() / ("readgen_wl_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    const auto cwd = fs::current_path();
    fs::current_path(tmp);
    const auto r = ValidateWorkloadFile(path.string());
    fs::current_path(cwd);
    fs::remove_all(tmp);
    ASSERT_TRUE(r.ok) << (r.issues.empty() ? "" : r.issues.front().field + ": " +
                                                     r.issues.front().message);
}

TEST(WorkloadSpec, BadSchemaVersion) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_schema_version.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "schema_version"));
}

TEST(WorkloadSpec, UnknownField) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_unknown_field.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "unexpected"));
}

TEST(WorkloadSpec, DuplicateTargetName) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_duplicate_target.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[1].name"));
}

TEST(WorkloadSpec, BadEndpointScheme) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_endpoint.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].endpoint"));
}

TEST(WorkloadSpec, UnsupportedAuthMode) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_auth_mode.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "auth.mode"));
}

TEST(WorkloadSpec, BadRateString) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_rate.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].target_rate"));
}

TEST(WorkloadSpec, ValidUncapped) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "valid_uncapped.json").string());
    ASSERT_TRUE(r.ok) << (r.issues.empty() ? "" : r.issues.front().field + ": " +
                                                     r.issues.front().message);
    ASSERT_EQ(r.resolved.targets.size(), 1u);
    EXPECT_EQ(r.resolved.targets[0].target_rate_bps, 0u);
    EXPECT_EQ(r.resolved.targets[0].pattern.max_bytes, 16ull << 20);
    EXPECT_FALSE(r.resolved.targets[0].pattern.max_bytes_auto);
    const auto cfg = ToRunConfig(r.resolved, r.resolved.targets[0]);
    EXPECT_EQ(cfg.target_rate_bps, 0u);
    EXPECT_EQ(cfg.max_bytes, 16ull << 20);
}

TEST(WorkloadSpec, UncappedRateTokens) {
    for (const char* rate : {"", "0", "0MBps", "uncapped", "Uncapped"}) {
        json j = json::parse(R"({
          "schema_version": 1,
          "run_id": "x",
          "duration": "30s",
          "auth": {"mode": "x509"},
          "targets": [{
            "name": "t0",
            "endpoint": "root://localhost:10945/",
            "filelist": "files.txt",
            "target_rate": "PLACEHOLDER",
            "max_inflight": 4,
            "pattern": {"type": "sequential", "read_size": "1MiB", "max_bytes": "8MiB"}
          }]
        })");
        j["targets"][0]["target_rate"] = rate;
        const auto r = ValidateWorkloadJson(j, FixtureDir().string());
        ASSERT_TRUE(r.ok) << rate << ": "
                          << (r.issues.empty() ? "" : r.issues.front().message);
        EXPECT_EQ(r.resolved.targets[0].target_rate_bps, 0u) << rate;
    }
}

TEST(WorkloadSpec, UncappedRejectsAutoMaxBytes) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_uncapped_auto.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].pattern.max_bytes"));
}

TEST(WorkloadSpec, MissingFilelist) {
    const auto r = ValidateWorkloadFile((FixtureDir() / "bad_missing_filelist.json").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].filelist"));
}

TEST(WorkloadSpec, JsonParseErrorReportsPath) {
    const fs::path tmp = fs::temp_directory_path() / ("readgen_badjson_" + std::to_string(::getpid()));
    {
        std::ofstream out(tmp);
        out << "{ not json";
    }
    const auto r = ValidateWorkloadFile(tmp.string());
    fs::remove(tmp);
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.issues.empty());
    EXPECT_EQ(r.issues.front().field, tmp.string());
    EXPECT_NE(r.issues.front().message.find("JSON parse error"), std::string::npos);
}

TEST(WorkloadSpec, MaxInflightOutOfRange) {
    json j = json::parse(R"({
      "schema_version": 1,
      "run_id": "x",
      "duration": "30s",
      "auth": {"mode": "x509"},
      "targets": [{
        "name": "t0",
        "endpoint": "root://localhost:10945/",
        "filelist": "files.txt",
        "max_inflight": 0,
        "pattern": {"type": "sequential"}
      }]
    })");
    const auto r = ValidateWorkloadJson(j, FixtureDir().string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].max_inflight"));
}

TEST(WorkloadSpec, RejectsUnknownFileFractionKey) {
    // file_fraction removed — unknown keys under pattern must fail validation.
    json j = json::parse(R"({
      "schema_version": 1,
      "run_id": "x",
      "duration": "30s",
      "auth": {"mode": "x509"},
      "targets": [{
        "name": "t0",
        "endpoint": "root://localhost:10945/",
        "filelist": "files.txt",
        "target_rate": "10MBps",
        "max_inflight": 4,
        "pattern": {"type": "sequential", "file_fraction": 0.5}
      }]
    })");
    const auto r = ValidateWorkloadJson(j, FixtureDir().string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasField(r, "targets[0].pattern.file_fraction"));
}

TEST(WorkloadSpec, ToRunConfigMapsFields) {
    const auto path = FixtureDir() / "valid_minimal.json";
    const auto r = ValidateWorkloadFile(path.string());
    ASSERT_TRUE(r.ok);
    const auto cfg = ToRunConfig(r.resolved, r.resolved.targets[0]);
    EXPECT_EQ(cfg.run_id, "fixture-valid");
    EXPECT_EQ(cfg.target, "t0");
    EXPECT_EQ(cfg.endpoint, "root://localhost:10945/");
    EXPECT_EQ(cfg.max_inflight, 4u);
    EXPECT_EQ(cfg.target_rate_bps, 10000000u);
    EXPECT_EQ(cfg.seed, 42u);
    EXPECT_TRUE(cfg.max_bytes_auto);
    EXPECT_FALSE(cfg.files.empty());
    EXPECT_FALSE(cfg.filelist_path.empty());
    EXPECT_DOUBLE_EQ(cfg.session_timeout_s, r.resolved.client_tuning.session_timeout_s);
    EXPECT_EQ(cfg.connection_window_s, r.resolved.client_tuning.connection_window_s);
    EXPECT_EQ(cfg.connection_retry, r.resolved.client_tuning.connection_retry);
    EXPECT_EQ(cfg.request_timeout_s, r.resolved.client_tuning.request_timeout_s);
    EXPECT_EQ(cfg.results_dir, r.resolved.sinks.results_dir);
    EXPECT_DOUBLE_EQ(cfg.snapshot_interval_s, r.resolved.sinks.snapshot_interval_s);
    EXPECT_EQ(cfg.job_id, r.resolved.sinks.job_id);
    EXPECT_EQ(cfg.write_results, r.resolved.sinks.write_results);
    EXPECT_EQ(cfg.pushgateway_url, r.resolved.sinks.pushgateway.url);
    EXPECT_EQ(cfg.pushgateway_job, r.resolved.sinks.pushgateway.job);
    EXPECT_EQ(cfg.pushgateway_keep, r.resolved.sinks.pushgateway.keep);
}

struct InvalidCase {
    const char* name;
    const char* file;
    const char* field;
};

class WorkloadInvalidFixture : public ::testing::TestWithParam<InvalidCase> {};

TEST_P(WorkloadInvalidFixture, RejectsWithField) {
    const auto p = GetParam();
    const auto r = ValidateWorkloadFile((FixtureDir() / p.file).string());
    EXPECT_FALSE(r.ok) << p.name;
    EXPECT_TRUE(HasField(r, p.field)) << p.name;
}

INSTANTIATE_TEST_SUITE_P(
    Table, WorkloadInvalidFixture,
    ::testing::Values(InvalidCase{"schema", "bad_schema_version.json", "schema_version"},
                      InvalidCase{"unknown", "bad_unknown_field.json", "unexpected"},
                      InvalidCase{"dup", "bad_duplicate_target.json", "targets[1].name"},
                      InvalidCase{"endpoint", "bad_endpoint.json", "targets[0].endpoint"},
                      InvalidCase{"auth", "bad_auth_mode.json", "auth.mode"},
                      InvalidCase{"rate", "bad_rate.json", "targets[0].target_rate"},
                      InvalidCase{"uncapped_auto", "bad_uncapped_auto.json",
                                  "targets[0].pattern.max_bytes"},
                      InvalidCase{"filelist", "bad_missing_filelist.json", "targets[0].filelist"}),
    [](const ::testing::TestParamInfo<InvalidCase>& info) {
        return std::string(info.param.name);
    });
