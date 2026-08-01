#include "readgen/workload_run_command.hh"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using readgen::RunConfig;
using readgen::RunWorkloadCommand;
using readgen::WorkloadRunOptions;
namespace fs = std::filesystem;

namespace {

fs::path MakeWorkloadDir(bool multi_target = false) {
    const auto dir =
        fs::temp_directory_path() / ("readgen_wlrun_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream fl(dir / "files.txt");
        fl << "/a.bin\n/b.bin\n";
    }
    {
        std::ofstream wl(dir / "wl.json");
        if (multi_target) {
            wl << R"({
  "schema_version": 1,
  "run_id": "wlrun-test",
  "duration": "30s",
  "seed": 7,
  "auth": {"mode": "x509"},
  "targets": [
    {
      "name": "t0",
      "endpoint": "root://localhost:10945/",
      "filelist": "files.txt",
      "target_rate": "80Mbps",
      "max_inflight": 4,
      "pattern": {"type": "sequential", "read_size": "1MiB", "max_bytes": "auto"}
    },
    {
      "name": "t1",
      "endpoint": "root://localhost:10946/",
      "filelist": "files.txt",
      "target_rate": "160Mbps",
      "max_inflight": 8,
      "pattern": {"type": "random"}
    }
  ],
  "client_tuning": {
    "session_timeout": "45s",
    "connection_window": 12,
    "connection_retry": 3,
    "request_timeout": 50
  },
  "sinks": {
    "results_dir": "out-results",
    "snapshot_interval": "5s",
    "job_id": "job-x",
    "write_results": true,
    "pushgateway": {"url": "http://pg.example:9091", "job": "rg", "keep": true}
  }
})";
        } else {
            wl << R"({
  "schema_version": 1,
  "run_id": "wlrun-test",
  "duration": "30s",
  "seed": 7,
  "auth": {"mode": "x509"},
  "targets": [{
    "name": "t0",
    "endpoint": "root://localhost:10945/",
    "filelist": "files.txt",
    "target_rate": "80Mbps",
    "max_inflight": 4,
    "pattern": {"type": "sequential", "read_size": "1MiB", "max_bytes": "auto"}
  }],
  "client_tuning": {
    "session_timeout": "45s",
    "connection_window": 12,
    "connection_retry": 3,
    "request_timeout": 50
  },
  "sinks": {
    "results_dir": "out-results",
    "snapshot_interval": "5s",
    "job_id": "job-x",
    "write_results": true,
    "pushgateway": {"url": "http://pg.example:9091", "job": "rg", "keep": true}
  }
})";
        }
    }
    return dir;
}

}  // namespace

TEST(WorkloadRunCommand, InvalidWorkloadExit2) {
    WorkloadRunOptions opts;
    opts.workload_path = "/no/such/workload.json";
    opts.skip_auth_check = true;
    opts.run_hook = [](const RunConfig&) { return 0; };
    EXPECT_EQ(RunWorkloadCommand(opts), 2);
}

TEST(WorkloadRunCommand, UnknownTargetExit2) {
    const auto dir = MakeWorkloadDir();
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.target = "missing";
    opts.run_hook = [](const RunConfig&) { return 0; };
    EXPECT_EQ(RunWorkloadCommand(opts), 2);
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, MultiTargetRequiresTarget) {
    const auto dir = MakeWorkloadDir(/*multi_target=*/true);
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    bool called = false;
    opts.run_hook = [&](const RunConfig&) {
        called = true;
        return 0;
    };
    EXPECT_EQ(RunWorkloadCommand(opts), 2);
    EXPECT_FALSE(called);
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, SingleTargetMapsConfig) {
    const auto dir = MakeWorkloadDir();
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    RunConfig seen;
    opts.run_hook = [&](const RunConfig& cfg) {
        seen = cfg;
        return 0;
    };
    EXPECT_EQ(RunWorkloadCommand(opts), 0);
    EXPECT_EQ(seen.run_id, "wlrun-test");
    EXPECT_EQ(seen.target, "t0");
    EXPECT_EQ(seen.endpoint, "root://localhost:10945/");
    EXPECT_EQ(seen.max_inflight, 4u);
    EXPECT_EQ(seen.target_rate_bytes_per_s, 10000000u);
    EXPECT_EQ(seen.seed, 7u);
    EXPECT_TRUE(seen.max_bytes_auto);
    EXPECT_EQ(seen.files.size(), 2u);
    EXPECT_DOUBLE_EQ(seen.session_timeout_s, 45.0);
    EXPECT_EQ(seen.connection_window_s, 12);
    EXPECT_EQ(seen.connection_retry, 3);
    EXPECT_EQ(seen.request_timeout_s, 50);
    EXPECT_EQ(seen.results_dir, "out-results");
    EXPECT_DOUBLE_EQ(seen.snapshot_interval_s, 5.0);
    EXPECT_EQ(seen.job_id, "job-x");
    EXPECT_TRUE(seen.write_results);
    EXPECT_EQ(seen.pushgateway_url, "http://pg.example:9091");
    EXPECT_EQ(seen.pushgateway_job, "rg");
    EXPECT_TRUE(seen.pushgateway_keep);
    EXPECT_FALSE(seen.dry_run);
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, ExplicitTargetAndDryRun) {
    const auto dir = MakeWorkloadDir(/*multi_target=*/true);
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.target = "t1";
    opts.dry_run = true;
    RunConfig seen;
    opts.run_hook = [&](const RunConfig& cfg) {
        seen = cfg;
        return 0;
    };
    EXPECT_EQ(RunWorkloadCommand(opts), 0);
    EXPECT_EQ(seen.target, "t1");
    EXPECT_EQ(seen.endpoint, "root://localhost:10946/");
    EXPECT_EQ(seen.max_inflight, 8u);
    EXPECT_TRUE(seen.dry_run);
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, AuthPreflightExit2WithoutSkip) {
    const auto dir = MakeWorkloadDir();
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = false;
    setenv("X509_USER_PROXY", (dir / "no-proxy").c_str(), 1);
    bool called = false;
    opts.run_hook = [&](const RunConfig&) {
        called = true;
        return 0;
    };
    EXPECT_EQ(RunWorkloadCommand(opts), 2);
    EXPECT_FALSE(called);
    unsetenv("X509_USER_PROXY");
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, SkipAuthCheckCallsHook) {
    const auto dir = MakeWorkloadDir();
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    setenv("X509_USER_PROXY", (dir / "no-proxy").c_str(), 1);
    bool called = false;
    opts.run_hook = [&](const RunConfig&) {
        called = true;
        return 0;
    };
    EXPECT_EQ(RunWorkloadCommand(opts), 0);
    EXPECT_TRUE(called);
    unsetenv("X509_USER_PROXY");
    fs::remove_all(dir);
}

TEST(WorkloadRunCommand, HookExitPropagated) {
    const auto dir = MakeWorkloadDir();
    WorkloadRunOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.run_hook = [](const RunConfig&) { return 1; };
    EXPECT_EQ(RunWorkloadCommand(opts), 1);
    fs::remove_all(dir);
}
