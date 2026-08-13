#!/usr/bin/env python3
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


RESULT_RE = re.compile(
    r"mode=(?P<mode>\S+)\s+bench=(?P<bench>\S+)\s+nodes=(?P<nodes>\d+)\s+"
    r"iters=(?P<iters>\d+)\s+total_ns=(?P<total_ns>\d+)\s+"
    r"build_ns=(?P<build_ns>\d+)\s+run_ns=(?P<run_ns>\d+)\s+"
    r"checksum=(?P<checksum>\d+)"
)


CASES = [
    {
        "case": "semtier_local",
        "mode": "semtier",
        "env": {"SEMTIER_FAST_NODE": "0", "SEMTIER_SLOW_NODE": "1"},
        "numactl": ["numactl", "--cpunodebind=0"],
        "expected_node": 0,
    },
    {
        "case": "semtier_remote",
        "mode": "semtier",
        "env": {"SEMTIER_FAST_NODE": "1", "SEMTIER_SLOW_NODE": "0"},
        "numactl": ["numactl", "--cpunodebind=0"],
        "expected_node": 1,
    },
    {
        "case": "malloc_local",
        "mode": "malloc",
        "env": {},
        "numactl": ["numactl", "--cpunodebind=0", "--membind=0"],
        "expected_node": 0,
    },
    {
        "case": "malloc_remote",
        "mode": "malloc",
        "env": {},
        "numactl": ["numactl", "--cpunodebind=0", "--membind=1"],
        "expected_node": 1,
    },
]


def repo_root():
    return Path(__file__).resolve().parents[1]


def run_checked(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, check=False)


def parse_numastat(output):
    values = {}
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("Total"):
            parts = stripped.split()
            if len(parts) >= 4:
                values["node0_mb"] = float(parts[1])
                values["node1_mb"] = float(parts[2])
                values["total_mb"] = float(parts[3])
            break
    return values


def collect_numastat(pid):
    proc = subprocess.run(
        ["numastat", "-p", str(pid)],
        text=True,
        capture_output=True,
        check=False,
    )
    parsed = parse_numastat(proc.stdout)
    parsed["numastat_rc"] = proc.returncode
    parsed["numastat_stdout"] = proc.stdout
    parsed["numastat_stderr"] = proc.stderr
    return parsed


def parse_result_line(line):
    match = RESULT_RE.search(line)
    if not match:
        return None
    result = match.groupdict()
    for key in ["nodes", "iters", "total_ns", "build_ns", "run_ns", "checksum"]:
        result[key] = int(result[key])
    result["total_s"] = result["total_ns"] / 1e9
    result["build_s"] = result["build_ns"] / 1e9
    result["run_s"] = result["run_ns"] / 1e9
    return result


def run_one(root, case, rep, args):
    env = os.environ.copy()
    env.update(case["env"])
    env["SEMTIER_EVENT_LOG"] = str(args.outdir / f"{case['case']}_rep{rep}.jsonl")
    if args.debug and case["mode"] == "semtier":
        env["SEMTIER_DEBUG"] = "1"
        env["SEMTIER_STRICT_POLICY"] = "1"

    cmd = (
        case["numactl"]
        + [
            str(root / "bench" / "pointer-chase"),
            "--mode",
            case["mode"],
            "--bench",
            args.bench,
            "--nodes",
            str(args.nodes),
            "--iters",
            str(args.iters),
            "--sleep-before-shutdown",
            str(args.sleep),
        ]
    )

    print(f"== {case['case']} rep {rep} ==", flush=True)
    print(" ".join(cmd), flush=True)
    proc = subprocess.Popen(
        cmd,
        cwd=root,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=1,
    )

    stdout_lines = []
    result = None
    numastat = {}
    assert proc.stdout is not None
    for line in proc.stdout:
        print(line, end="", flush=True)
        stdout_lines.append(line)
        parsed = parse_result_line(line)
        if parsed:
            result = parsed
            time.sleep(args.numastat_delay)
            numastat = collect_numastat(proc.pid)

    stderr = proc.stderr.read() if proc.stderr else ""
    rc = proc.wait()
    if stderr:
        sys.stderr.write(stderr)
        sys.stderr.flush()

    row = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "case": case["case"],
        "rep": rep,
        "pid": proc.pid,
        "returncode": rc,
        "expected_node": case["expected_node"],
        "cmd": " ".join(cmd),
        "stderr": stderr.strip(),
    }
    if result:
        row.update(result)
    row.update({k: v for k, v in numastat.items() if not k.endswith("_stdout") and not k.endswith("_stderr")})
    row["placement_ok"] = placement_ok(row, case["expected_node"])
    row["stdout"] = "".join(stdout_lines).strip()
    row["numastat_stdout"] = numastat.get("numastat_stdout", "")
    row["numastat_stderr"] = numastat.get("numastat_stderr", "")
    return row


def placement_ok(row, expected_node):
    node0 = row.get("node0_mb")
    node1 = row.get("node1_mb")
    if node0 is None or node1 is None:
        return ""
    if expected_node == 0:
        return node0 > node1
    if expected_node == 1:
        return node1 > node0
    return ""


def write_outputs(rows, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    json_path = outdir / "matrix_results.json"
    csv_path = outdir / "matrix_results.csv"
    json_path.write_text(json.dumps(rows, indent=2))

    fieldnames = [
        "timestamp_utc",
        "case",
        "rep",
        "pid",
        "returncode",
        "mode",
        "bench",
        "nodes",
        "iters",
        "total_ns",
        "build_ns",
        "run_ns",
        "total_s",
        "build_s",
        "run_s",
        "node0_mb",
        "node1_mb",
        "total_mb",
        "expected_node",
        "placement_ok",
        "checksum",
        "numastat_rc",
        "cmd",
        "stderr",
    ]
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {csv_path}")
    print(f"wrote {json_path}")


def print_summary(rows):
    groups = {}
    for row in rows:
        if row.get("returncode") != 0 or "run_s" not in row:
            continue
        groups.setdefault(row["case"], []).append(row)

    print("\nsummary:")
    for case, items in groups.items():
        run_vals = [item["run_s"] for item in items]
        node0_vals = [item.get("node0_mb", 0.0) for item in items]
        node1_vals = [item.get("node1_mb", 0.0) for item in items]
        placement = [str(item.get("placement_ok", "")) for item in items]
        print(
            f"{case:16s} run_s_avg={sum(run_vals)/len(run_vals):.4f} "
            f"node0_avg={sum(node0_vals)/len(node0_vals):.2f}MB "
            f"node1_avg={sum(node1_vals)/len(node1_vals):.2f}MB "
            f"placement={','.join(placement)}"
        )

    if "malloc_local" in groups and "malloc_remote" in groups:
        local = sum(item["run_s"] for item in groups["malloc_local"]) / len(groups["malloc_local"])
        remote = sum(item["run_s"] for item in groups["malloc_remote"]) / len(groups["malloc_remote"])
        print(f"malloc_remote/local={remote/local:.3f}x")
    if "semtier_local" in groups and "semtier_remote" in groups:
        local = sum(item["run_s"] for item in groups["semtier_local"]) / len(groups["semtier_local"])
        remote = sum(item["run_s"] for item in groups["semtier_remote"]) / len(groups["semtier_remote"])
        print(f"semtier_remote/local={remote/local:.3f}x")


def main():
    parser = argparse.ArgumentParser(description="Collect SemTier NUMA matrix data.")
    parser.add_argument("--nodes", type=int, default=5_000_000)
    parser.add_argument("--iters", type=int, default=20)
    parser.add_argument("--bench", choices=["chase", "stream"], default="chase")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--sleep", type=int, default=5)
    parser.add_argument("--numastat-delay", type=float, default=0.5)
    parser.add_argument("--debug", action="store_true", help="enable SemTier mbind debug/strict mode")
    parser.add_argument(
        "--outdir",
        type=Path,
        default=repo_root() / "results" / "collection",
        help="directory for CSV/JSON/event logs",
    )
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    args.outdir = args.outdir.resolve()

    root = repo_root()
    if not args.no_build:
        build = run_checked(["make"], cwd=root)
        if build.stdout:
            print(build.stdout, end="")
        if build.stderr:
            print(build.stderr, end="", file=sys.stderr)
        if build.returncode != 0:
            raise SystemExit(build.returncode)

    args.outdir.mkdir(parents=True, exist_ok=True)
    rows = []
    for rep in range(1, args.reps + 1):
        for case in CASES:
            rows.append(run_one(root, case, rep, args))
            write_outputs(rows, args.outdir)

    print_summary(rows)


if __name__ == "__main__":
    main()
