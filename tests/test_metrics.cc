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
    r.ObserveSessionOk(1000, 2, 0.01, 0.02, 0.5, 1.0);
    r.ObserveSessionFail(ErrorClass::Connection);
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
    EXPECT_EQ(s.workers_configured, 8u);
    EXPECT_EQ(s.errors_by_class.at("connection"), 1u);
    EXPECT_EQ(s.open_seconds.count, 1u);
    EXPECT_GE(s.cpu_seconds_total, 0.0);
}

TEST(ProcessSample, ProcSelfAvailable) {
    auto s = SampleProcess();
    // On Linux CI/dev boxes this should be non-zero after process startup.
    EXPECT_GE(s.cpu_seconds, 0.0);
    EXPECT_GT(s.rss_bytes, 0u);
}
