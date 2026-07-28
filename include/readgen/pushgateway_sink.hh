#ifndef READGEN_PUSHGATEWAY_SINK_HH
#define READGEN_PUSHGATEWAY_SINK_HH

#include "readgen/metrics.hh"

#include <string>

namespace readgen {

// Pushes Prometheus text to a Pushgateway.
// Grouping key: job=<push_job>, instance=<job_id from snapshot>.
// Deletes the grouping key on Finish() (clean exit).
class PushgatewaySink {
public:
    // base_url e.g. http://xrdmon.cern.ch:9091  (no trailing path required)
    PushgatewaySink(std::string base_url, std::string push_job = "xrd-readgen");

    // PUT current snapshot. Returns false on HTTP/transport failure (logged).
    bool Push(const MetricsSnapshot& snap);

    // DELETE the grouping key. Safe to call multiple times.
    void Finish(const std::string& instance);

    const std::string& base_url() const { return base_url_; }

private:
    std::string GroupUrl(const std::string& instance) const;
    bool HttpRequest(const char* method, const std::string& url, const std::string& body,
                     long* http_code_out);

    std::string base_url_;
    std::string push_job_;
    std::string last_instance_;
    bool finished_ = false;
};

}  // namespace readgen

#endif  // READGEN_PUSHGATEWAY_SINK_HH
