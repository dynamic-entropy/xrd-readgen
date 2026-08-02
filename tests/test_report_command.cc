#include "readgen/report_command.hh"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using json = nlohmann::json;
using readgen::AggregateResultFiles;
using readgen::DiscoverResultFiles;
using readgen::ReportOptions;
using readgen::RunReportCommand;
namespace fs = std::filesystem;

namespace {

json MakeResult(const std::string& run_id, const std::string& job_id, uint64_t bytes,
                uint64_t ok, uint64_t fail, double elapsed, const std::string& site,
                const std::string& server) {
    json by_ds = json::object();
    by_ds[server] = {{"bytes_read", bytes},
                     {"sessions_ok", ok},
                     {"sessions_fail", fail},
                     {"errors", json::object()},
                     {"cms_site", site}};
    json by_site = json::object();
    by_site[site] = {{"bytes_read", bytes},
                     {"sessions_ok", ok},
                     {"sessions_fail", fail},
                     {"errors", json::object()}};
    return {{"run_id", run_id},
            {"job_id", job_id},
            {"target", "default"},
            {"endpoint", "root://example/"},
            {"elapsed_s", elapsed},
            {"bytes_read", bytes},
            {"sessions_ok", ok},
            {"sessions_fail", fail},
            {"ops", ok},
            {"errors", json::object()},
            {"soft_faults", json::object()},
            {"by_data_server", by_ds},
            {"by_cms_site", by_site}};
}

void WriteResult(const fs::path& path, const json& j) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << j.dump(2) << '\n';
}

}  // namespace

class ReportCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("readgen_report_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
};

TEST_F(ReportCommandTest, DiscoversSingleRunDir) {
    const auto result = dir_ / "global" / "result.json";
    WriteResult(result, MakeResult("global", "host-a", 1000, 2, 0, 10.0, "SITE_A", "ds1:1094"));

    ReportOptions opts;
    opts.path = (dir_ / "global").string();
    const auto files = DiscoverResultFiles(opts);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], result.string());
}

TEST_F(ReportCommandTest, DiscoversResultJsonFile) {
    const auto result = dir_ / "global" / "result.json";
    WriteResult(result, MakeResult("global", "host-a", 1000, 2, 0, 10.0, "SITE_A", "ds1:1094"));

    ReportOptions opts;
    opts.path = result.string();
    const auto files = DiscoverResultFiles(opts);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], result.string());
}

TEST_F(ReportCommandTest, DiscoversFleetViaResultsDir) {
    WriteResult(dir_ / "i1" / "global" / "result.json",
                MakeResult("global", "host-i1", 1000, 1, 0, 8.0, "SITE_A", "ds1:1094"));
    WriteResult(dir_ / "i2" / "global" / "result.json",
                MakeResult("global", "host-i2", 3000, 3, 1, 12.0, "SITE_B", "ds2:1094"));

    ReportOptions opts;
    opts.results_dir = dir_.string();
    opts.run_id = "global";
    const auto files = DiscoverResultFiles(opts);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0], (dir_ / "i1" / "global" / "result.json").string());
    EXPECT_EQ(files[1], (dir_ / "i2" / "global" / "result.json").string());
}

TEST_F(ReportCommandTest, FleetFlagIgnoresSingleLayout) {
    WriteResult(dir_ / "global" / "result.json",
                MakeResult("global", "host-a", 1000, 1, 0, 5.0, "SITE_A", "ds1:1094"));
    WriteResult(dir_ / "i1" / "global" / "result.json",
                MakeResult("global", "host-i1", 2000, 2, 0, 6.0, "SITE_A", "ds1:1094"));

    ReportOptions opts;
    opts.results_dir = dir_.string();
    opts.run_id = "global";
    opts.fleet = true;
    const auto files = DiscoverResultFiles(opts);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], (dir_ / "i1" / "global" / "result.json").string());
}

TEST_F(ReportCommandTest, PrefersSingleOverFleetWhenBothExist) {
    WriteResult(dir_ / "global" / "result.json",
                MakeResult("global", "host-a", 1000, 1, 0, 5.0, "SITE_A", "ds1:1094"));
    WriteResult(dir_ / "i1" / "global" / "result.json",
                MakeResult("global", "host-i1", 2000, 2, 0, 6.0, "SITE_A", "ds1:1094"));

    ReportOptions opts;
    opts.results_dir = dir_.string();
    opts.run_id = "global";
    const auto files = DiscoverResultFiles(opts);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], (dir_ / "global" / "result.json").string());
}

TEST_F(ReportCommandTest, AggregatesFleetBytesAndSites) {
    const auto p1 = dir_ / "i1" / "global" / "result.json";
    const auto p2 = dir_ / "i2" / "global" / "result.json";
    WriteResult(p1, MakeResult("global", "host-i1", 1000, 1, 0, 8.0, "SITE_A", "ds1:1094"));
    WriteResult(p2, MakeResult("global", "host-i2", 3000, 3, 1, 12.0, "SITE_A", "ds1:1094"));

    const auto summary = AggregateResultFiles({p1.string(), p2.string()});
    EXPECT_TRUE(summary.fleet);
    EXPECT_EQ(summary.run_id, "global");
    EXPECT_EQ(summary.bytes_read, 4000u);
    EXPECT_EQ(summary.sessions_ok, 4u);
    EXPECT_EQ(summary.sessions_fail, 1u);
    EXPECT_DOUBLE_EQ(summary.elapsed_s, 12.0);
    ASSERT_EQ(summary.by_cms_site.count("SITE_A"), 1u);
    EXPECT_EQ(summary.by_cms_site.at("SITE_A").bytes_read, 4000u);
    ASSERT_EQ(summary.by_data_server.count("ds1:1094"), 1u);
    EXPECT_EQ(summary.by_data_server.at("ds1:1094").sessions_ok, 4u);
    EXPECT_EQ(summary.job_ids.size(), 2u);
}

TEST_F(ReportCommandTest, RunReportJsonExit0) {
    WriteResult(dir_ / "global" / "result.json",
                MakeResult("global", "host-a", 5000, 5, 0, 10.0, "SITE_A", "ds1:1094"));

    ReportOptions opts;
    opts.path = (dir_ / "global").string();
    opts.json = true;
    EXPECT_EQ(RunReportCommand(opts), 0);
}

TEST_F(ReportCommandTest, MissingResultsExit2) {
    ReportOptions opts;
    opts.results_dir = dir_.string();
    opts.run_id = "missing";
    EXPECT_EQ(RunReportCommand(opts), 2);
}
