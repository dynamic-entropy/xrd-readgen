#include "readgen/probe_command.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using readgen::FileSessionOptions;
using readgen::FileSessionResult;
using readgen::ProbeOptions;
using readgen::RunProbeCommand;
namespace fs = std::filesystem;

namespace {

fs::path MakeWorkloadDir() {
    const auto dir = fs::temp_directory_path() / ("readgen_probe_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream fl(dir / "files.txt");
        fl << "/a.bin\n/b.bin\n/c.bin\n";
    }
    {
        std::ofstream wl(dir / "wl.json");
        wl << R"({
  "schema_version": 1,
  "run_id": "probe-test",
  "duration": "30s",
  "auth": {"mode": "x509"},
  "targets": [{
    "name": "t0",
    "endpoint": "root://localhost:10945/",
    "filelist": "files.txt",
    "pattern": {"type": "sequential"}
  }]
})";
    }
    return dir;
}

}  // namespace

TEST(ProbeCommand, InvalidWorkloadExit2) {
    ProbeOptions opts;
    opts.workload_path = "/no/such/workload.json";
    opts.skip_auth_check = true;
    EXPECT_EQ(RunProbeCommand(opts), 2);
}

TEST(ProbeCommand, AllOkExit0) {
    const auto dir = MakeWorkloadDir();
    std::atomic<int> starts{0};
    ProbeOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.concurrency = 2;
    opts.json = true;
    opts.session_runner = [&](const FileSessionOptions& o, readgen::FileSessionDone done) {
        ++starts;
        FileSessionResult r;
        r.ok = true;
        r.url = o.url;
        r.open_ms = 1.0;
        r.ttfb_ms = 2.0;
        r.open_hosts = 1;
        r.data_server = "localhost:10945";
        std::thread([done = std::move(done), r]() mutable { done(r); }).detach();
    };
    EXPECT_EQ(RunProbeCommand(opts), 0);
    EXPECT_EQ(starts.load(), 3);
    fs::remove_all(dir);
}

TEST(ProbeCommand, LimitAndTarget) {
    const auto dir = MakeWorkloadDir();
    std::atomic<int> starts{0};
    ProbeOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.target = "t0";
    opts.limit = 1;
    opts.session_runner = [&](const FileSessionOptions& o, readgen::FileSessionDone done) {
        ++starts;
        FileSessionResult r;
        r.ok = true;
        r.url = o.url;
        std::thread([done = std::move(done), r]() mutable { done(r); }).detach();
    };
    EXPECT_EQ(RunProbeCommand(opts), 0);
    EXPECT_EQ(starts.load(), 1);
    fs::remove_all(dir);
}

TEST(ProbeCommand, UnknownTargetExit2) {
    const auto dir = MakeWorkloadDir();
    ProbeOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.target = "missing";
    opts.session_runner = [](const FileSessionOptions&, readgen::FileSessionDone) {};
    EXPECT_EQ(RunProbeCommand(opts), 2);
    fs::remove_all(dir);
}

TEST(ProbeCommand, ProbeFailureExit1) {
    const auto dir = MakeWorkloadDir();
    ProbeOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = true;
    opts.limit = 1;
    opts.session_runner = [](const FileSessionOptions& o, readgen::FileSessionDone done) {
        FileSessionResult r;
        r.ok = false;
        r.url = o.url;
        r.error = "open failed: not authorized";
        r.err_code = 3010;
        std::thread([done = std::move(done), r]() mutable { done(r); }).detach();
    };
    EXPECT_EQ(RunProbeCommand(opts), 1);
    fs::remove_all(dir);
}

TEST(ProbeCommand, AuthPreflightExit2WithoutSkip) {
    const auto dir = MakeWorkloadDir();
    ProbeOptions opts;
    opts.workload_path = (dir / "wl.json").string();
    opts.skip_auth_check = false;
    // Point at a missing proxy via env so default path is irrelevant.
    setenv("X509_USER_PROXY", (dir / "no-proxy").c_str(), 1);
    opts.session_runner = [](const FileSessionOptions&, readgen::FileSessionDone) {};
    EXPECT_EQ(RunProbeCommand(opts), 2);
    unsetenv("X509_USER_PROXY");
    fs::remove_all(dir);
}
