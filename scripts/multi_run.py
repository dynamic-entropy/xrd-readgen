#!/usr/bin/env python3
"""Fork N concurrent uncapped xrd-readgen processes (one XrdCl PostMaster each).

Fleet labelling (Grafana):
  All children share one --run_id: {prefix}-n{N}-mi{max_inflight}
  Each child has a unique --job-id (Pushgateway instance): {hostname}-i{i}
  FileSink dirs are per-instance so result.json does not collide:
    {results_dir}/i{i}/{run_id}/
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


ERROR_LINE_RE = re.compile(
    r"Auth failed|timeout|FATAL|error:|No servers|\[Error", re.IGNORECASE
)


def _die(msg: str, code: int = 2) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(code)


def _resolve_binary(explicit: Optional[str]) -> str:
    if explicit:
        return explicit
    env = os.environ.get("XRD_READGEN")
    if env:
        return env
    which = subprocess.run(
        "command -v xrd-readgen", shell=True, capture_output=True, text=True
    )
    path = which.stdout.strip()
    if path:
        return path
    for candidate in (Path("./build/xrd-readgen"), Path("./xrd-readgen")):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    _die("xrd-readgen not found (set --binary or XRD_READGEN)")


def _ensure_proxy(skip: bool, dry_run: bool) -> Optional[str]:
    if dry_run or skip:
        return None
    proxy = os.environ.get("X509_USER_PROXY", "")
    if not proxy:
        uid = os.getuid()
        candidate = f"/tmp/x509up_u{uid}"
        if Path(candidate).is_file():
            proxy = candidate
            os.environ["X509_USER_PROXY"] = proxy
    if not proxy:
        _die(
            f"X509_USER_PROXY unset and /tmp/x509up_u{os.getuid()} missing — "
            "export a proxy (or pass --skip-auth-check for local only)"
        )
    p = Path(proxy)
    if not p.is_file():
        _die(f"proxy file not found: {proxy}")
    if not os.access(p, os.R_OK):
        _die(f"proxy not readable: {proxy}")
    os.environ["X509_USER_PROXY"] = proxy
    return proxy


def _host_short() -> str:
    try:
        return socket.gethostname().split(".")[0]
    except OSError:
        return "localhost"


def _load_result(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        with path.open() as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def _tail_errors(log_path: Path, n: int = 6) -> List[str]:
    if not log_path.is_file():
        return []
    try:
        text = log_path.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return []
    hits = [ln for ln in text.splitlines() if ERROR_LINE_RE.search(ln)]
    if hits:
        return hits[-n:]
    lines = text.splitlines()
    return lines[-8:] if lines else []


def build_child_argv(
    *,
    binary: str,
    endpoint: str,
    filelist: str,
    duration: str,
    max_inflight: int,
    chunk_size: str,
    max_bytes: str,
    session_timeout: str,
    connection_window: int,
    run_id: str,
    job_id: str,
    results_dir: str,
    pushgateway: str,
    pushgateway_job: str,
    dry_run: bool,
    no_sitename: bool,
) -> List[str]:
    cmd = [
        binary,
        "run",
        "--endpoint",
        endpoint,
        "--filelist",
        filelist,
        "--duration",
        duration,
        "--max-inflight",
        str(max_inflight),
        "--chunk-size",
        chunk_size,
        "--max-bytes",
        max_bytes,
        "--session-timeout",
        session_timeout,
        "--connection-window",
        str(connection_window),
        "--run-id",
        run_id,
        "--job-id",
        job_id,
        "--results-dir",
        results_dir,
        "--snapshot-interval",
        "5s",
    ]
    if pushgateway:
        cmd += ["--pushgateway", pushgateway, "--pushgateway-job", pushgateway_job]
    if dry_run:
        cmd.append("--dry-run")
    if no_sitename:
        cmd.append("--no-sitename-query")
    return cmd


def run_fleet(
    *,
    n: int,
    endpoint: str,
    filelist: str,
    max_bytes: str,
    max_inflight: int = 16,
    duration: str = "10m",
    chunk_size: str = "2MB",
    pushgateway: str = "",
    pushgateway_job: str = "xrd-readgen",
    results_dir: str = "results",
    run_prefix: str = "multi",
    binary: Optional[str] = None,
    session_timeout: str = "90s",
    connection_window: int = 15,
    dry_run: bool = False,
    no_sitename: bool = False,
    skip_auth_check: bool = False,
    print_summary: bool = True,
) -> Dict[str, Any]:
    """Spawn N processes; return a structured fleet summary dict."""
    if n < 1:
        _die("--n must be a positive integer")
    if max_inflight < 1:
        _die("--max-inflight must be a positive integer")
    binary_path = _resolve_binary(binary)
    if not os.access(binary_path, os.X_OK):
        _die(f"binary not executable: {binary_path}")
    if not Path(filelist).is_file():
        _die(f"filelist not found: {filelist}")

    proxy = _ensure_proxy(skip_auth_check, dry_run)
    results_root = Path(results_dir)
    results_root.mkdir(parents=True, exist_ok=True)

    host = _host_short()
    fleet_run_id = f"{run_prefix}-n{n}-mi{max_inflight}"

    if print_summary:
        print(f"==> multi-run: N={n} max_inflight/proc={max_inflight} duration={duration}")
        print(f"    binary:   {binary_path}")
        print(f"    endpoint: {endpoint}")
        print(f"    filelist: {filelist}")
        print(f"    chunk:    {chunk_size}  max_bytes: {max_bytes}")
        print(f"    results:  {results_root}/i*/{fleet_run_id}/")
        print(f"    run_id:   {fleet_run_id}  (shared across all {n} processes)")
        print(f"    prefix:   {run_prefix}")
        if dry_run:
            print("    auth:     (dry-run)")
        elif skip_auth_check:
            print("    auth:     skipped (--skip-auth-check)")
        else:
            print(f"    auth:     X509_USER_PROXY={proxy}")

    procs: List[subprocess.Popen] = []
    meta: List[Dict[str, Any]] = []

    def _kill_all(signum: int = signal.SIGTERM) -> None:
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signum)
                except (ProcessLookupError, PermissionError):
                    try:
                        p.send_signal(signum)
                    except ProcessLookupError:
                        pass

    def _on_signal(signum: int, _frame: Any) -> None:
        _kill_all(signum)

    prev_int = signal.signal(signal.SIGINT, _on_signal)
    prev_term = signal.signal(signal.SIGTERM, _on_signal)

    try:
        for i in range(1, n + 1):
            job_id = f"{host}-i{i}"
            inst_results = results_root / f"i{i}"
            inst_results.mkdir(parents=True, exist_ok=True)
            log_path = results_root / f"{fleet_run_id}-i{i}.log"
            argv = build_child_argv(
                binary=binary_path,
                endpoint=endpoint,
                filelist=filelist,
                duration=duration,
                max_inflight=max_inflight,
                chunk_size=chunk_size,
                max_bytes=max_bytes,
                session_timeout=session_timeout,
                connection_window=connection_window,
                run_id=fleet_run_id,
                job_id=job_id,
                results_dir=str(inst_results),
                pushgateway=pushgateway,
                pushgateway_job=pushgateway_job,
                dry_run=dry_run,
                no_sitename=no_sitename,
            )
            if print_summary:
                print(f"    start i={i} run_id={fleet_run_id} job_id={job_id} log={log_path}")
            log_f = open(log_path, "w")  # noqa: SIM115 — kept open for child lifetime
            proc = subprocess.Popen(
                argv,
                stdout=log_f,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            procs.append(proc)
            meta.append(
                {
                    "i": i,
                    "job_id": job_id,
                    "log_path": str(log_path),
                    "result_dir": str(inst_results / fleet_run_id),
                    "log_f": log_f,
                    "argv": argv,
                }
            )

        for proc, m in zip(procs, meta):
            m["exit"] = proc.wait()
            m["log_f"].close()
            del m["log_f"]
    finally:
        signal.signal(signal.SIGINT, prev_int)
        signal.signal(signal.SIGTERM, prev_term)

    processes: List[Dict[str, Any]] = []
    fleet_sum_bps = 0.0
    sum_count = 0
    sessions_ok = 0
    sessions_fail = 0
    errors_merged: Dict[str, int] = {}
    worst = 0

    for m in meta:
        result_json = Path(m["result_dir"]) / "result.json"
        data = _load_result(result_json)
        bps = data.get("achieved_bps")
        mbps = None
        if isinstance(bps, (int, float)):
            mbps = float(bps) / 1e6
            fleet_sum_bps += float(bps)
            sum_count += 1
        ok = int(data.get("sessions_ok") or 0)
        fail = int(data.get("sessions_fail") or 0)
        sessions_ok += ok
        sessions_fail += fail
        errs = data.get("errors") or {}
        if isinstance(errs, dict):
            for k, v in errs.items():
                errors_merged[str(k)] = errors_merged.get(str(k), 0) + int(v)
        ec = int(m["exit"])
        worst = max(worst, ec)
        entry = {
            "i": m["i"],
            "job_id": m["job_id"],
            "exit": ec,
            "achieved_bps": bps,
            "achieved_MBps": mbps,
            "sessions_ok": ok,
            "sessions_fail": fail,
            "errors": errs if isinstance(errs, dict) else {},
            "result_json": str(result_json),
            "log_path": m["log_path"],
        }
        processes.append(entry)

    summary: Dict[str, Any] = {
        "run_id": fleet_run_id,
        "n_procs": n,
        "max_inflight": max_inflight,
        "total_session_cap": n * max_inflight,
        "endpoint": endpoint,
        "filelist": filelist,
        "chunk_size": chunk_size,
        "max_bytes": max_bytes,
        "duration": duration,
        "fleet_achieved_bps": fleet_sum_bps if sum_count else None,
        "fleet_achieved_MBps": (fleet_sum_bps / 1e6) if sum_count else None,
        "result_count": sum_count,
        "sessions_ok": sessions_ok,
        "sessions_fail": sessions_fail,
        "fail_rate": (
            sessions_fail / (sessions_ok + sessions_fail)
            if (sessions_ok + sessions_fail) > 0
            else None
        ),
        "errors": errors_merged,
        "exit": worst,
        "processes": processes,
        "utc_end": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    if print_summary:
        print()
        print(
            f"==> summary  (run_id={fleet_run_id}; per-process rates are independent — "
            "sum ≈ site push)"
        )
        print(
            f"{'i':<4} {'job_id':<28} {'exit':<6} {'achieved_MBps':<14} "
            f"{'ok/fail':<12} errors"
        )
        for p in processes:
            mbps_s = "n/a" if p["achieved_MBps"] is None else f"{p['achieved_MBps']:.2f}"
            errs = p["errors"]
            errs_s = (
                ",".join(f"{a}:{b}" for a, b in errs.items()) if errs else "-"
            )
            print(
                f"{p['i']:<4} {p['job_id']:<28} {p['exit']:<6} {mbps_s:<14} "
                f"{p['sessions_ok']}/{p['sessions_fail']:<10} {errs_s}"
            )
            if p["exit"] != 0:
                print(f"      log tail ({p['log_path']}):")
                for ln in _tail_errors(Path(p["log_path"])):
                    print(f"        {ln}")
        if sum_count:
            print(
                f"    fleet_sum_achieved_MBps≈{fleet_sum_bps / 1e6:.2f}  "
                f"(from {sum_count}/{n} result.json)"
            )
        print(
            f"    D1: select run_id={fleet_run_id}, instance=All — "
            "Achieved panel has per-instance + total"
        )
        print(f"    logs: {results_root}/{fleet_run_id}-i*.log")
        if worst == 1:
            print(
                "    hint: exit 1 = all FileSessions failed.",
                file=sys.stderr,
            )
            print(
                "          check errors in result.json (auth vs timeout). "
                "Auth → export X509_USER_PROXY.",
                file=sys.stderr,
            )
            print(
                "          timeout under high N×max_inflight → back off "
                "(try --n 1 --max-inflight 8..64 first).",
                file=sys.stderr,
            )

    return summary


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Fork N concurrent uncapped xrd-readgen runs (fleet cell)."
    )
    p.add_argument("--n", type=int, required=True, help="Number of concurrent processes")
    p.add_argument("--endpoint", required=True, help="root:// endpoint")
    p.add_argument("--filelist", required=True, help="One path per line")
    p.add_argument(
        "--max-bytes",
        required=True,
        help="Per-session byte cap (uncapped mode; required)",
    )
    p.add_argument(
        "--max-inflight",
        type=int,
        default=16,
        help="In-flight sessions per process (default: 16)",
    )
    p.add_argument("--duration", default="10m", help="Run duration (default: 10m)")
    p.add_argument("--chunk-size", default="2MB", help="Read chunk size (default: 2MB)")
    p.add_argument("--pushgateway", default="", help="Pushgateway base URL")
    p.add_argument(
        "--pushgateway-job", default="xrd-readgen", help="Pushgateway job label"
    )
    p.add_argument("--results-dir", default="results", help="FileSink root")
    p.add_argument("--run-prefix", default="multi", help="run_id prefix")
    p.add_argument("--binary", default=None, help="xrd-readgen binary path")
    p.add_argument("--session-timeout", default="90s", help="Per-session timeout")
    p.add_argument(
        "--connection-window",
        type=int,
        default=15,
        help="XrdCl ConnectionWindow seconds",
    )
    p.add_argument("--dry-run", action="store_true", help="Pass --dry-run to each child")
    p.add_argument(
        "--no-sitename-query", action="store_true", help="Disable sitename queries"
    )
    p.add_argument(
        "--skip-auth-check",
        action="store_true",
        help="Allow missing X509_USER_PROXY (local only)",
    )
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    summary = run_fleet(
        n=args.n,
        endpoint=args.endpoint,
        filelist=args.filelist,
        max_bytes=args.max_bytes,
        max_inflight=args.max_inflight,
        duration=args.duration,
        chunk_size=args.chunk_size,
        pushgateway=args.pushgateway,
        pushgateway_job=args.pushgateway_job,
        results_dir=args.results_dir,
        run_prefix=args.run_prefix,
        binary=args.binary,
        session_timeout=args.session_timeout,
        connection_window=args.connection_window,
        dry_run=args.dry_run,
        no_sitename=args.no_sitename_query,
        skip_auth_check=args.skip_auth_check,
    )
    return int(summary.get("exit") or 0)


if __name__ == "__main__":
    raise SystemExit(main())
