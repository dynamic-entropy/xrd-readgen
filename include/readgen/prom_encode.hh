#ifndef READGEN_PROM_ENCODE_HH
#define READGEN_PROM_ENCODE_HH

#include "readgen/metrics.hh"

#include <string>

namespace readgen {

// Encode a MetricsSnapshot as Prometheus text exposition (0.0.4).
// Common labels: run_id, job_id, target, endpoint (from the snapshot).
// Histograms use cumulative _bucket{le="..."} + _sum + _count.
std::string EncodePrometheusText(const MetricsSnapshot& snap);

}  // namespace readgen

#endif  // READGEN_PROM_ENCODE_HH
