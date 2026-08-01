#!/usr/bin/env python3
"""Capacity study driver: Axis A (N) and Axis B (max_inflight) via multi_run."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence
from urllib.parse import urlparse


def _load_multi_run():
    here = Path(__file__).resolve().parent
    path = here / "multi_run.py"
    spec = importlib.util.spec_from_file_location("multi_run", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


multi_run = _load_multi_run()


def endpoint_slug(endpoint: str) -> str:
    """Short tag for run_prefix / run_id (e.g. fnal, eoscms, ral)."""
    host = urlparse(endpoint).hostname or endpoint
    host = host.lower()
    if "fnal" in host:
        return "fnal"
    if "eoscms" in host or host.endswith("cern.ch"):
        if "eoscms" in host:
            return "eoscms"
        return "cern"
    if "gridpp.rl" in host or "rl.ac.uk" in host:
        return "ral"
    # first DNS label, strip non-alnum
    label = host.split(".")[0]
    return re.sub(r"[^a-z0-9]+", "", label)[:24] or "ep"


def parse_int_list(s: str) -> List[int]:
    out: List[int] = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        v = int(part)
        if v < 1:
            raise argparse.ArgumentTypeError(f"values must be >= 1, got {v}")
        out.append(v)
    if not out:
        raise argparse.ArgumentTypeError("list must not be empty")
    return out


def cell_to_row(cell: Dict[str, Any]) -> Dict[str, Any]:
    per = cell.get("processes") or []
    mbps_list = [
        p["achieved_MBps"]
        for p in per
        if isinstance(p.get("achieved_MBps"), (int, float))
    ]
    errs = cell.get("errors") or {}
    timeout = int(errs.get("timeout", 0)) if isinstance(errs, dict) else 0
    return {
        "axis": cell.get("axis", ""),
        "cell": cell.get("cell", ""),
        "run_id": cell.get("run_id", ""),
        "n_procs": cell.get("n_procs", ""),
        "max_inflight": cell.get("max_inflight", ""),
        "total_session_cap": cell.get("total_session_cap", ""),
        "endpoint": cell.get("endpoint", ""),
        "fleet_achieved_MBps": (
            f"{cell['fleet_achieved_MBps']:.2f}"
            if isinstance(cell.get("fleet_achieved_MBps"), (int, float))
            else ""
        ),
        "per_proc_min_MBps": f"{min(mbps_list):.2f}" if mbps_list else "",
        "per_proc_max_MBps": f"{max(mbps_list):.2f}" if mbps_list else "",
        "sessions_ok": cell.get("sessions_ok", ""),
        "sessions_fail": cell.get("sessions_fail", ""),
        "fail_rate": (
            f"{cell['fail_rate']:.4f}"
            if isinstance(cell.get("fail_rate"), (int, float))
            else ""
        ),
        "timeouts": timeout,
        "errors": (
            ",".join(f"{k}:{v}" for k, v in sorted(errs.items()))
            if isinstance(errs, dict)
            else ""
        ),
        "exit": cell.get("exit", ""),
        "utc_start": cell.get("utc_start", ""),
        "utc_end": cell.get("utc_end", ""),
    }


CSV_COLUMNS = [
    "axis",
    "cell",
    "run_id",
    "n_procs",
    "max_inflight",
    "total_session_cap",
    "endpoint",
    "fleet_achieved_MBps",
    "per_proc_min_MBps",
    "per_proc_max_MBps",
    "sessions_ok",
    "sessions_fail",
    "fail_rate",
    "timeouts",
    "errors",
    "exit",
    "utc_start",
    "utc_end",
]


def write_summary(out_dir: Path, cells: Sequence[Dict[str, Any]], knobs: Dict[str, Any]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = [cell_to_row(c) for c in cells]

    csv_path = out_dir / "summary.csv"
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    md_path = out_dir / "summary.md"
    lines = [
        "# Capacity sweep summary",
        "",
        "## Knobs",
        "",
        f"- endpoint: `{knobs.get('endpoint', '')}`",
        f"- filelist: `{knobs.get('filelist', '')}`",
        f"- chunk_size: `{knobs.get('chunk_size', '')}`",
        f"- max_bytes: `{knobs.get('max_bytes', '')}`",
        f"- duration: `{knobs.get('duration', '')}`",
        f"- study_id: `{knobs.get('study_id', '')}`",
        "",
        "## Reading tip",
        "",
        "- **Axis A** varies process count `N` at fixed `max_inflight` "
        "(more PostMasters / TCP channels).",
        "- **Axis B** varies `max_inflight` at high `N` "
        "(concurrent outstanding reads per process). "
        "Fewer in-flight sessions can raise fleet throughput when timeouts dominate.",
        "",
        "## Results",
        "",
        "| axis | cell | run_id | N | mi | fleet MB/s | ok/fail | fail_rate | timeouts | exit |",
        "|---|---|---|---:|---:|---:|---|---:|---:|---:|",
    ]
    for r in rows:
        lines.append(
            f"| {r['axis']} | {r['cell']} | `{r['run_id']}` | {r['n_procs']} | "
            f"{r['max_inflight']} | {r['fleet_achieved_MBps']} | "
            f"{r['sessions_ok']}/{r['sessions_fail']} | {r['fail_rate']} | "
            f"{r['timeouts']} | {r['exit']} |"
        )
    lines.append("")
    md_path.write_text("\n".join(lines) + "\n")
    print(f"wrote {csv_path}")
    print(f"wrote {md_path}")


def load_cells(out_dir: Path) -> List[Dict[str, Any]]:
    cells = []
    for path in sorted(out_dir.glob("**/cell.json")):
        try:
            cells.append(json.loads(path.read_text()))
        except (OSError, json.JSONDecodeError) as e:
            print(f"warn: skip {path}: {e}", file=sys.stderr)
    return cells


def cmd_summarize(args: argparse.Namespace) -> int:
    out_dir = Path(args.out_dir)
    cells = load_cells(out_dir)
    if not cells:
        print(f"error: no cell.json under {out_dir}", file=sys.stderr)
        return 2
    knobs = {
        "endpoint": cells[0].get("endpoint", ""),
        "filelist": cells[0].get("filelist", ""),
        "chunk_size": cells[0].get("chunk_size", ""),
        "max_bytes": cells[0].get("max_bytes", ""),
        "duration": cells[0].get("duration", ""),
        "study_id": args.study_id or cells[0].get("study_id", ""),
    }
    write_summary(out_dir, cells, knobs)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    slug = endpoint_slug(args.endpoint)
    study = args.study_id or f"{slug}-cap"

    axis_a = [(n, args.axis_a_max_inflight) for n in args.n_list]
    # Axis B: skip the (N, mi) already covered by Axis A at fixed mi
    axis_b = [
        (args.axis_b_n, mi)
        for mi in args.max_inflight_list
        if not (mi == args.axis_a_max_inflight and args.axis_b_n in args.n_list)
    ]

    plan = [("A", n, mi) for n, mi in axis_a] + [("B", n, mi) for n, mi in axis_b]
    print(f"==> capacity_sweep: {len(plan)} cells → {out_dir}")
    for axis, n, mi in plan:
        print(f"    {axis}: N={n} max_inflight={mi}")

    cells: List[Dict[str, Any]] = []
    knobs = {
        "endpoint": args.endpoint,
        "filelist": args.filelist,
        "chunk_size": args.chunk_size,
        "max_bytes": args.max_bytes,
        "duration": args.duration,
        "study_id": study,
    }

    for axis, n, mi in plan:
        cell_name = f"{axis}-n{n}-mi{mi}"
        prefix = f"{study}-{slug}"
        utc_start = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        print(f"\n==> cell {cell_name}  prefix={prefix}")
        summary = multi_run.run_fleet(
            n=n,
            endpoint=args.endpoint,
            filelist=args.filelist,
            max_bytes=args.max_bytes,
            max_inflight=mi,
            duration=args.duration,
            chunk_size=args.chunk_size,
            pushgateway=args.pushgateway,
            pushgateway_job=args.pushgateway_job,
            results_dir=args.results_dir,
            run_prefix=prefix,
            binary=args.binary,
            session_timeout=args.session_timeout,
            connection_window=args.connection_window,
            dry_run=args.dry_run,
            no_sitename=args.no_sitename_query,
            skip_auth_check=args.skip_auth_check,
            print_summary=True,
        )
        cell = dict(summary)
        cell["axis"] = axis
        cell["cell"] = cell_name
        cell["study_id"] = study
        cell["utc_start"] = utc_start
        cell_dir = out_dir / cell_name
        cell_dir.mkdir(parents=True, exist_ok=True)
        # Drop non-serializable noise
        cell.pop("processes", None)
        cell["processes"] = summary.get("processes", [])
        (cell_dir / "cell.json").write_text(json.dumps(cell, indent=2) + "\n")
        cells.append(cell)
        if not args.continue_on_error and int(cell.get("exit") or 0) not in (0,):
            # exit 1 is "all sessions failed" — still record and continue study
            # only abort on launcher errors (2+)
            if int(cell.get("exit") or 0) >= 2:
                print(f"error: cell {cell_name} exit {cell['exit']}; aborting", file=sys.stderr)
                write_summary(out_dir, cells, knobs)
                return int(cell["exit"])

    write_summary(out_dir, cells, knobs)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Capacity sweep (Axis A/B) over multi_run fleets")
    sub = p.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="Execute Axis A then Axis B; write summary")
    run.add_argument("--endpoint", required=True)
    run.add_argument("--filelist", required=True)
    run.add_argument("--max-bytes", default="32MB")
    run.add_argument("--chunk-size", default="4MB")
    run.add_argument("--duration", default="5m")
    run.add_argument("--pushgateway", default="")
    run.add_argument("--pushgateway-job", default="xrd-readgen")
    run.add_argument("--results-dir", default="/var/lib/xrd-readgen/results")
    run.add_argument("--out-dir", required=True, help="Study output (summary + cell.json)")
    run.add_argument("--study-id", default="", help="Prefix tag (default: {slug}-cap)")
    run.add_argument(
        "--n-list",
        type=parse_int_list,
        default=[1, 2, 4, 8, 16, 30],
        help="Axis A process counts (comma-separated)",
    )
    run.add_argument(
        "--max-inflight-list",
        type=parse_int_list,
        default=[8, 16, 32],
        help="Axis B max_inflight values (comma-separated)",
    )
    run.add_argument(
        "--axis-a-max-inflight",
        type=int,
        default=16,
        help="Fixed max_inflight for Axis A (default 16)",
    )
    run.add_argument(
        "--axis-b-n",
        type=int,
        default=30,
        help="Fixed N for Axis B (default 30)",
    )
    run.add_argument("--binary", default=None)
    run.add_argument("--session-timeout", default="90s")
    run.add_argument("--connection-window", type=int, default=15)
    run.add_argument("--dry-run", action="store_true")
    run.add_argument("--no-sitename-query", action="store_true")
    run.add_argument("--skip-auth-check", action="store_true")
    run.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Continue after launcher exit >= 2 (default: abort)",
    )
    run.set_defaults(func=cmd_run)

    sm = sub.add_parser("summarize", help="Re-render summary.md/csv from cell.json files")
    sm.add_argument("--out-dir", required=True)
    sm.add_argument("--study-id", default="")
    sm.set_defaults(func=cmd_summarize)

    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
