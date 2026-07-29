# Grafana dashboards

Importable JSON for ops Grafana (xrdmon test / CMS prod). The observability
stack is **not** shipped in this repo — only dashboard definitions.

| File | Story |
|---|---|
| [`xrd-readgen-d1.json`](xrd-readgen-d1.json) | **D1 Challenge Overview** — achieved vs target, success rate, inflight, hard errors, **soft faults**, open/TTFB, bytes/CPU-sec |

**Hard vs soft:** `readgen_errors_total` = failed sessions; `readgen_soft_faults_total` = XrdCl Error log lines (e.g. connection reset) even when the session still completes ok.

## Import (xrdmon)

1. Open Grafana (e.g. `http://xrdmon.cern.ch:3000`).
2. **Dashboards → New → Import** → upload `xrd-readgen-d1.json` (or paste).
3. Select the Prometheus datasource that scrapes the Pushgateway.
4. Variables: `job` (default `xrd-readgen`), `instance`, `run_id`.

Re-import (or overwrite) after dashboard JSON updates.

## Generator side

```bash
./dev/local-server.sh -b
./build/xrd-readgen run ... \
  --pushgateway http://xrdmon.cern.ch:9091 \
  --job-id "$(hostname -s)" \
  --run-id demo
```

Soft-fault counting requires XrdCl Error logs to be emitted (default / `XRD_LOGLEVEL=Info` is fine).

### Throughput on D1

Prefer `readgen_achieved_rate_bytes` (gauge): **cumulative**
`bytes_read / elapsed` on the generator’s `steady_clock` (same as run-summary
achieved). Stall bounds (`ConnectionWindow` / session timeout) keep drain tails
short so elapsed stays a fair denominator.

The achieved-vs-target panel uses **Y soft-min = 0** so a flat local run near
target is not auto-zoomed into a fake “huge” oscillation. Rate panels use Grafana
unit `Bps` (SI **MB/s**). Prefer CLI `--rate 35MBps` / `1Gbps` (SI); `MiBps` is
accepted but the banner echoes the parsed SI rate (`41.94 MB/s (from '40MiBps')`).
**Bytes per CPU-second** uses `decbytes` (bytes, not Bps) so a value like 303 MB
means efficiency, not a 303 MB/s network rate.

Defaults that bound stuck peers: `--session-timeout 60s`,
`--connection-window 15`, `--connection-retry 2` (XrdCl ConnectionWindow default is 120).

Re-import D1 after JSON updates. Use `--snapshot-interval` ≤ scrape interval (often 15s).

**Ground truth:** `results/<dir>/<run_id>/result.json` and `metrics.jsonl`.
If JSONL holds the target but Grafana does not, re-import this D1 JSON.
