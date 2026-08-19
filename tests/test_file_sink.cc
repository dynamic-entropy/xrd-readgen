#include "readgen/file_sink.hh"
#include "readgen/metrics.hh"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using readgen::FileSink;
using readgen::Histogram;
using readgen::MetricsRegistry;
using readgen::RunInfoMeta;
namespace fs = std::filesystem;
using json = nlohmann::json;

class FileSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("readgen_sink_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
};

TEST_F(FileSinkTest, WritesJsonlAndResult) {
    MetricsRegistry reg;
    reg.SetLabels("t1", "j1", "default", "root://localhost:10945/");
    reg.SetConfigGauges(10 * 1024 * 1024, 4);
    reg.ObserveSessionOk(1024 * 1024, 4, 0.005, 0.01, 0.2, 2.0, "localhost:10945");
    reg.SampleProc();

    RunInfoMeta meta;
    meta.version = "0.1.0";
    meta.arch = "test";
    meta.xrdcl_version = "test";
    meta.seed = 42;
    meta.pattern = "sequential";

    FileSink sink(dir_.string(), "t1", meta);
    sink.Start();
    auto snap = reg.Snapshot(1.5);
    sink.WriteSnapshot(snap);
    sink.WriteResult(snap, 0.0);

    const auto jsonl_path = dir_ / "t1" / "metrics.jsonl";
    const auto result_path = dir_ / "t1" / "result.json";
    ASSERT_TRUE(fs::exists(jsonl_path));
    ASSERT_TRUE(fs::exists(result_path));

    std::ifstream jl(jsonl_path);
    std::string line;
    ASSERT_TRUE(std::getline(jl, line));
    auto j = json::parse(line);
    EXPECT_EQ(j["run_id"], "t1");
    EXPECT_EQ(j["readgen_bytes_read_total"], 1024 * 1024);
    EXPECT_TRUE(j.contains("readgen_process_resident_memory_bytes"));
    ASSERT_TRUE(j["readgen_open_seconds"].contains("counts"));
    ASSERT_TRUE(j["readgen_open_seconds"].contains("bounds"));
    ASSERT_TRUE(j["readgen_open_seconds"].contains("sum"));
    EXPECT_EQ(j["readgen_open_seconds"]["count"], 1);
    ASSERT_TRUE(j.contains("by_data_server"));
    EXPECT_EQ(j["by_data_server"]["localhost:10945"]["bytes_read"], 1024 * 1024);

    std::ifstream rf(result_path);
    auto result = json::parse(rf);
    EXPECT_EQ(result["run_id"], "t1");
    EXPECT_EQ(result["job_id"], "j1");
    EXPECT_EQ(result["bytes_read"], 1024 * 1024);
    EXPECT_TRUE(result["latency"]["open_seconds"].contains("p50"));
    EXPECT_TRUE(result.contains("readgen_bytes_per_cpu_second"));
    EXPECT_TRUE(result.contains("readgen_process_resident_memory_bytes"));
    EXPECT_EQ(result["run_info"]["seed"], 42);
    ASSERT_TRUE(result.contains("by_data_server"));
    EXPECT_EQ(result["by_data_server"]["localhost:10945"]["bytes_read"], 1024 * 1024);
}

TEST_F(FileSinkTest, WritesWorkloadArtifacts) {
    MetricsRegistry reg;
    reg.SetLabels("wl1", "j1", "aaa", "root://example/");
    reg.SetConfigGauges(0, 2);
    reg.ObserveSessionOk(1024, 1, 0.001, 0.002, 0.01, 1.0);

    RunInfoMeta meta;
    meta.version = "0.1.0";
    meta.arch = "test";
    meta.xrdcl_version = "test";
    meta.seed = 7;
    meta.pattern = "sequential";
    meta.schema_version = 1;
    meta.auth_mode = "x509";
    meta.workload_hash = "abc123";
    meta.workload_resolved_json = "{\"schema_version\":1}\n";

    FileSink sink(dir_.string(), "wl1", meta);
    sink.Start();
    auto snap = reg.Snapshot(1.0);
    sink.WriteResult(snap, 0.0);

    const auto run_dir = dir_ / "wl1";
    ASSERT_TRUE(fs::exists(run_dir / "workload_resolved.json"));
    ASSERT_TRUE(fs::exists(run_dir / "workload.hash"));
    ASSERT_TRUE(fs::exists(run_dir / "result.json"));

    std::ifstream hash_in(run_dir / "workload.hash");
    std::string hash_line;
    ASSERT_TRUE(std::getline(hash_in, hash_line));
    EXPECT_EQ(hash_line, "abc123");

    std::ifstream rf(run_dir / "result.json");
    auto result = json::parse(rf);
    EXPECT_EQ(result["workload_hash"], "abc123");
    EXPECT_EQ(result["run_info"]["schema_version"], 1);
    EXPECT_EQ(result["run_info"]["auth_mode"], "x509");
}
