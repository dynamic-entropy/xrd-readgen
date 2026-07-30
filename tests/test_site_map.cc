#include "readgen/site_map.hh"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <unistd.h>

using readgen::HostnameOnly;
using readgen::SiteMap;
namespace fs = std::filesystem;

class SiteMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("readgen_sitemap_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
};

TEST(HostnameOnly, StripsPort) {
    EXPECT_EQ(HostnameOnly("cms.example.org:1094"), "cms.example.org");
    EXPECT_EQ(HostnameOnly("cms.example.org"), "cms.example.org");
    EXPECT_EQ(HostnameOnly("[2001:db8::1]:1094"), "[2001:db8::1]");
}

TEST_F(SiteMapTest, LookupExactAndHostname) {
    SiteMap map;
    map.Add("cmsxrootd.fnal.gov", "T1_US_FNAL");
    EXPECT_EQ(map.Lookup("cmsxrootd.fnal.gov:1094"), "T1_US_FNAL");
    EXPECT_EQ(map.Lookup("CMSXROOTD.FNAL.GOV"), "T1_US_FNAL");
    EXPECT_EQ(map.Lookup("other.example:1094"), "");
}

TEST_F(SiteMapTest, SuffixWildcard) {
    SiteMap map;
    map.Add("*.fnal.gov", "T1_US_FNAL");
    map.Add("*.cern.ch", "T2_CH_CERN");
    map.Add("special.cern.ch", "T2_CH_CERN_SPECIAL");
    EXPECT_EQ(map.Lookup("cmsstor204.fnal.gov:21115"), "T1_US_FNAL");
    EXPECT_EQ(map.Lookup("st-096-gg500sda.cern.ch:1095"), "T2_CH_CERN");
    EXPECT_EQ(map.Lookup("special.cern.ch:1095"), "T2_CH_CERN_SPECIAL");
    EXPECT_EQ(map.Lookup("disk.example.org:1094"), "");
}

TEST_F(SiteMapTest, LongestSuffixWins) {
    SiteMap map;
    map.Add("*.gov", "TOO_BROAD");
    map.Add("*.fnal.gov", "T1_US_FNAL");
    EXPECT_EQ(map.Lookup("cmsstor901.fnal.gov:31696"), "T1_US_FNAL");
}

TEST_F(SiteMapTest, LoadFile) {
    const auto path = dir_ / "sites.json";
    {
        std::ofstream out(path);
        out << R"({"xrd.cern.ch":"T2_CH_CERN","xrd.cern.ch:1094":"T2_CH_CERN"})";
    }
    const SiteMap map = SiteMap::LoadFile(path.string());
    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map.Lookup("xrd.cern.ch:1095"), "T2_CH_CERN");
}

TEST_F(SiteMapTest, LoadFileRejectsBadRoot) {
    const auto path = dir_ / "bad.json";
    {
        std::ofstream out(path);
        out << "[1,2,3]";
    }
    EXPECT_THROW(SiteMap::LoadFile(path.string()), std::runtime_error);
}
