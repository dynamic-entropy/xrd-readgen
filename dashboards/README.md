# Grafana dashboards

Importable JSON for ops Grafana (xrdmon test / CMS prod). The observability
stack is **not** shipped in this repo — only dashboard definitions.

| File | Story |
|---|---|
| [`xrd-readgen-d1.json`](xrd-readgen-d1.json) | **D1 Challenge Overview** — achieved vs target, success rate, inflight, hard/soft errors, open/TTFB, bytes/CPU-sec, **achieved rate + FileSessions by CMS site and DataServer** |

**Hard vs soft:** `readgen_errors_total` = failed sessions; `readgen_soft_faults_total` = XrdCl Error log lines (e.g. connection reset) even when the session still completes ok.

**Attribution (Phase 3A):** Site/server throughput uses the same definition as
the overall panel: gauges `readgen_site_achieved_rate_bytes` /
`readgen_endpoint_achieved_rate_bytes` (`bytes / wall`). Do **not** prefer
`rate(*_bytes_total)` here — Pushgateway scrapes make PromQL `rate()` noisy /
false-zero when a label is idle. FileSessions panels count completed
Open→…→Close work items — **not** TCP connections.

**Capacity mode (uncapped):** When `readgen_target_rate_bytes` is 0, the
Achieved-vs-target panel plots **achieved only** (target series filtered out).
Configure `max_inflight` / `read_size` / `max_bytes` and read the achieved gauge /
run summary — see `workloads/example_uncapped.json`. Multi-process fleets share a
**stable** `run_id` (do not encode N/mi in the id). Achieved-vs-target: per-instance
series stacked (`stack1`); **Total** is a second `sum()` query drawn as an
unstacked line (`stacking: Off`, group `total`) so it rides the stack top without
double-counting. On exit the generator pushes idle zero-rate gauges so Achieved
rate does not stick at the last sample.

## Import (xrdmon)

1. Open Grafana (e.g. `http://xrdmon.cern.ch:3000`).
2. **Dashboards → New → Import** → upload `xrd-readgen-d1.json` (or paste).
3. Select the Prometheus datasource that scrapes the Pushgateway.
4. Variables: `job` (default `xrd-readgen`), `instance` (multi/All), `run_id` (multi/All — default All so every run overlays on one plot).

Re-import (or overwrite) after dashboard JSON updates.

## Generator side

```bash
./dev/local-server.sh -b
./build/xrd-readgen run ... \
  --pushgateway http://xrdmon.cern.ch:9091 \
  --job-id "$(hostname -s)" \
  --run-id demo
```

Soft-fault counting requires XrdCl Error logs (default / `XRD_LOGLEVEL=Info`).
Site panels fill from live `query config sitename` (deferred off the I/O path;
cached per DataServer). Optional `--site-map` is fallback only.

### Throughput on D1

Prefer `readgen_achieved_rate_bytes` (gauge): **cumulative**
`bytes_read / elapsed` on the generator’s `steady_clock` (same as run-summary
achieved). Stall bounds (`ConnectionWindow` / session timeout) keep drain tails
short so elapsed stays a fair denominator.

The achieved-vs-target panel uses **Y soft-min = 0** so a flat local run near
target is not auto-zoomed into a fake “huge” oscillation. Rate panels use Grafana
unit `bps` (SI **bits/s**, shown as Mbps/Gbps). Prom gauges are bytes/s;
panel queries multiply by 8. CLI `--rate` accepts SI bits only (`1Gbps`,
`280Mbps`). Banner / run summary print bits.
**Bytes per CPU-second** uses `decbytes` (bytes, not network rate) so a value
like 303 MB means efficiency, not a 303 Mbps link rate.

Defaults that bound stuck peers: `--session-timeout 60s`,
`--connection-window 15`, `--connection-retry 2` (XrdCl ConnectionWindow default is 120).

Re-import D1 after JSON updates. Use `--snapshot-interval` ≤ scrape interval (often 15s).

**Ground truth:** `results/<dir>/<run_id>/result.json` and `metrics.jsonl`
(`by_data_server` / `by_cms_site`).
