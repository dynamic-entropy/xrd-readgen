#include "readgen/metrics.hh"

#include <gtest/gtest.h>

#include <cmath>
#include <thread>
#include <vector>

using readgen::ErrorClass;
using readgen::Histogram;
using readgen::HistogramPercentile;
using readgen::MetricsRegistry;
using readgen::SampleProcess;

TEST(Histogram, ObserveAndCount) {
    Histogram h;
    h.Observe(0.001);
    h.Observe(0.5);
    h.Observe(100.0);  // +Inf bucket
    auto s = h.Snapshot();
    EXPECT_EQ(s.count, 3u);
    EXPECT_EQ(s.counts.size(), readgen::kHistogramBuckets);
    EXPECT_GT(s.sum, 100.0);
    uint64_t total = 0;
    for (auto c : s.counts) total += c;
    EXPECT_EQ(total, 3u);
}

TEST(Histogram, Percentile) {
    Histogram h;
    for (int i = 0; i < 100; ++i) h.Observe(0.01);  // all in 10ms bucket
    auto s = h.Snapshot();
    EXPECT_NEAR(HistogramPercentile(s, 0.50), 0.01, 0.01);
    EXPECT_NEAR(HistogramPercentile(s, 0.95), 0.01, 0.01);
}

TEST(Histogram, ThreadSafeObserve) {
    Histogram h;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) h.Observe(0.001 * (i % 10 + 1));
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(h.Snapshot().count, 4000u);
}

TEST(MetricsRegistry, SessionOkAndFail) {
    MetricsRegistry r;
    r.SetLabels("run1", "job1", "default", "root://localhost/");
    r.SetConfigGauges(1024 * 1024, 8);
    r.ObserveSessionOk(1000, 2, 0.01, 0.02, 0.5, 1.0, "ds1.example:1094");
    r.ObserveSessionFail(ErrorClass::Connection, "ds1.example:1094");
    r.SetInflight(3, 5);
    r.SampleProc();

    auto s = r.Snapshot(1.0);
    EXPECT_EQ(s.run_id, "run1");
    EXPECT_EQ(s.job_id, "job1");
    EXPECT_EQ(s.bytes_read_total, 1000u);
    EXPECT_EQ(s.sessions_ok, 1u);
    EXPECT_EQ(s.sessions_fail, 1u);
    EXPECT_EQ(s.read_ops_total, 2u);
    EXPECT_EQ(s.peak_inflight, 5u);
    EXPECT_EQ(s.max_inflight, 8u);
    EXPECT_EQ(s.errors_by_class.at("connection"), 1u);
    EXPECT_EQ(s.open_seconds.count, 1u);
    EXPECT_GE(s.cpu_seconds_total, 0.0);

    ASSERT_EQ(s.by_data_server.size(), 1u);
    const auto& ep = s.by_data_server.at("ds1.example:1094");
    EXPECT_EQ(ep.bytes_read, 1000u);
    EXPECT_EQ(ep.sessions_ok, 1u);
    EXPECT_EQ(ep.sessions_fail, 1u);
    EXPECT_EQ(ep.errors_by_class.at("connection"), 1u);
    EXPECT_TRUE(ep.cms_site.empty());
    EXPECT_TRUE(s.by_cms_site.empty());
}

TEST(MetricsRegistry, AttributionWithSiteMap) {
    readgen::SiteMap map;
    map.Add("ds-a.example.org", "T2_AA");
    map.Add("ds-b.example.org", "T2_BB");

    MetricsRegistry r;
    r.SetLabels("run1", "job1", "default", "root://global/");
    r.SetSiteMap(&map);
    r.ObserveSessionOk(100, 1, 0.01, 0.02, 0.1, 1.0, "ds-a.example.org:1094");
    r.ObserveSessionOk(50, 1, 0.01, 0.02, 0.1, 1.0, "ds-a.example.org:1094");
    r.ObserveSessionOk(200, 1, 0.01, 0.02, 0.1, 1.0, "ds-b.example.org:1094");
    r.ObserveSessionFail(ErrorClass::Timeout, "ds-b.example.org:1094");
    r.ObserveSessionOk(10, 1, 0.01, 0.02, 0.1, 1.0, "unmapped.example:1094");

    auto s = r.Snapshot(2.0);
    ASSERT_EQ(s.by_data_server.size(), 3u);
    EXPECT_EQ(s.by_data_server.at("ds-a.example.org:1094").cms_site, "T2_AA");
    EXPECT_EQ(s.by_data_server.at("ds-a.example.org:1094").bytes_read, 150u);
    EXPECT_EQ(s.by_data_server.at("ds-b.example.org:1094").cms_site, "T2_BB");
    EXPECT_EQ(s.by_data_server.at("unmapped.example:1094").cms_site, "");

    ASSERT_EQ(s.by_cms_site.size(), 2u);
    EXPECT_EQ(s.by_cms_site.at("T2_AA").bytes_read, 150u);
    EXPECT_EQ(s.by_cms_site.at("T2_AA").sessions_ok, 2u);
    EXPECT_EQ(s.by_cms_site.at("T2_BB").bytes_read, 200u);
    EXPECT_EQ(s.by_cms_site.at("T2_BB").sessions_fail, 1u);
}

TEST(MetricsRegistry, EmptyDataServerUsesUnknown) {
    MetricsRegistry r;
    r.ObserveSessionFail(ErrorClass::NotFound, "");
    auto s = r.Snapshot(1.0);
    ASSERT_TRUE(s.by_data_server.count(readgen::kUnknownDataServer));
    EXPECT_EQ(s.by_data_server.at(readgen::kUnknownDataServer).sessions_fail, 1u);
}

TEST(ProcessSample, ProcSelfAvailable) {
    auto s = SampleProcess();
    // On Linux CI/dev boxes this should be non-zero after process startup.
    EXPECT_GE(s.cpu_seconds, 0.0);
    EXPECT_GT(s.rss_bytes, 0u);
}
