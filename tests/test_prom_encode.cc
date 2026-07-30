#include "readgen/metrics.hh"
#include "readgen/prom_encode.hh"

#include <gtest/gtest.h>

#include <string>

using readgen::EncodePrometheusText;
using readgen::ErrorClass;
using readgen::MetricsRegistry;

TEST(PromEncode, ContainsCoreSeriesAndHistogramBuckets) {
    MetricsRegistry reg;
    reg.SetLabels("run-a", "host1", "default", "root://localhost/");
    reg.SetConfigGauges(10 * 1024 * 1024, 4);
    reg.ObserveSessionOk(1024, 2, 0.01, 0.02, 0.5, 1.0, "srv-a:1094");
    reg.ObserveSessionFail(ErrorClass::Timeout, "srv-a:1094");
    reg.SetInflight(1, 2);
    reg.SampleProc();

    auto snap1 = reg.Snapshot(1.25);
    EXPECT_NEAR(snap1.achieved_rate_bytes, 1024.0 / 1.25, 1e-6);

    reg.ObserveSessionOk(1024, 1, 0.01, 0.02, 0.5, 1.0, "srv-b:1094");
    auto snap2 = reg.Snapshot(2.25);
    EXPECT_NEAR(snap2.wall_s, 2.25, 1e-9);
    EXPECT_NEAR(snap2.achieved_rate_bytes, 2048.0 / 2.25, 1e-6);

    const std::string text = EncodePrometheusText(snap2);
    EXPECT_NE(text.find("readgen_bytes_read_total{"), std::string::npos);
    EXPECT_NE(text.find("run_id=\"run-a\""), std::string::npos);
    EXPECT_NE(text.find("job_id=\"host1\""), std::string::npos);
    EXPECT_NE(text.find("readgen_sessions_total{"), std::string::npos);
    EXPECT_NE(text.find("result=\"ok\""), std::string::npos);
    EXPECT_NE(text.find("result=\"fail\""), std::string::npos);
    EXPECT_NE(text.find("# TYPE readgen_open_seconds histogram"), std::string::npos);
    EXPECT_NE(text.find("readgen_open_seconds_bucket{"), std::string::npos);
    EXPECT_NE(text.find("le=\"+Inf\""), std::string::npos);
    EXPECT_NE(text.find("readgen_open_seconds_sum{"), std::string::npos);
    EXPECT_NE(text.find("readgen_open_seconds_count{"), std::string::npos);
    EXPECT_NE(text.find("class=\"timeout\""), std::string::npos);
    EXPECT_NE(text.find("readgen_target_rate_bytes{"), std::string::npos);
    EXPECT_NE(text.find("readgen_achieved_rate_bytes{"), std::string::npos);
    EXPECT_NE(text.find("readgen_endpoint_bytes_total{"), std::string::npos);
    EXPECT_NE(text.find("readgen_endpoint_achieved_rate_bytes{"), std::string::npos);
    EXPECT_NE(text.find("data_server=\"srv-a:1094\""), std::string::npos);
    EXPECT_NE(text.find("data_server=\"srv-b:1094\""), std::string::npos);
    EXPECT_NE(text.find("readgen_endpoint_sessions_total{"), std::string::npos);
}
