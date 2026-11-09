#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Automated validation & evidence archiver for OpenRR-ASI (openrr-amd.asi).
Runs test_asi_units.exe + 3-run same-process A/B openrr_bench.exe (1080p, 1440p, 4K + 4K Legacy),
archives raw runtime logs and summary.json into docs/test-evidence/runs/<date>-<tag>-<sha>/,
and writes a standalone snapshot in docs/test-evidence/history/.
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys

def run_cmd(cmd):
    print(f"$ {' '.join(cmd)}", flush=True)
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    print(res.stdout, end="", flush=True)
    if res.returncode != 0:
        raise RuntimeError(f"Command failed (exit {res.returncode}): {' '.join(cmd)}")
    return res.stdout

def main():
    ap = argparse.ArgumentParser(description="Run OpenRR-ASI validation suite and archive evidence cleanly.")
    ap.add_argument("--tag", required=True, help="Stage/phase slug, e.g. phase2-fp8-input")
    ap.add_argument("--baseline-runtime", default="build/asi/base_d6f70fb/plugins/openrr-amd.asi")
    ap.add_argument("--baseline-data-dir", default="build/asi/base_d6f70fb/openrr-amd")
    ap.add_argument("--runtime", default="build/asi/stage/plugins/openrr-amd.asi")
    ap.add_argument("--data-dir", default="build/asi/stage/openrr-amd")
    ap.add_argument("--dlssd", default="C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll")
    args = ap.parse_args()

    sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
    date_str = datetime.date.today().isoformat()
    run_folder_name = f"{date_str}-{args.tag}-{sha}"
    runs_dir = os.path.join("docs", "test-evidence", "runs", run_folder_name)
    hist_dir = os.path.join("docs", "test-evidence", "history")
    os.makedirs(runs_dir, exist_ok=True)
    os.makedirs(hist_dir, exist_ok=True)

    # Clean any stale bench-* temp directories in build/asi
    for entry in os.listdir("build/asi") if os.path.isdir("build/asi") else []:
        if entry.startswith("bench-"):
            shutil.rmtree(os.path.join("build/asi", entry), ignore_errors=True)

    unit_out = run_cmd([os.path.abspath("build/asi/test_asi_units.exe")])

    bench_exe = os.path.abspath("build/asi/openrr_bench.exe")
    resolutions = [
        ("1080p", "1280", "720", "1920", "1080"),
        ("1440p", "1706", "960", "2560", "1440"),
        ("4k",    "2560", "1440", "3840", "2160"),
    ]

    bench_sections = []
    for r in range(1, 4):
        lines = [f"### Run {r} / 3"]
        for name, rw, rh, ow, oh in resolutions:
            sub_out = os.path.join(runs_dir, f"bench-ab-{name}-r{r}")
            out_txt = run_cmd([
                bench_exe,
                "--baseline-runtime", args.baseline_runtime,
                "--baseline-data-dir", args.baseline_data_dir,
                "--runtime", args.runtime,
                "--data-dir", args.data_dir,
                "--dlssd", args.dlssd,
                "--rw", rw, "--rh", rh, "--ow", ow, "--oh", oh,
                "--fast-mode", "1",
                "--out", sub_out,
            ])
            lines.append(out_txt.strip())
        bench_sections.append("\n".join(lines))

    leg_out_dir = os.path.join(runs_dir, "bench-ab-4k-leg")
    leg_txt = run_cmd([
        bench_exe,
        "--baseline-runtime", args.baseline_runtime,
        "--baseline-data-dir", args.baseline_data_dir,
        "--runtime", args.runtime,
        "--data-dir", args.data_dir,
        "--dlssd", args.dlssd,
        "--rw", "2560", "--rh", "1440", "--ow", "3840", "--oh", "2160",
        "--fast-mode", "0",
        "--out", leg_out_dir,
    ])
    bench_sections.append("### Legacy Mode (`--fast-mode 0`, 4K Quality)\n" + leg_txt.strip())

    existing_hist = sorted([f for f in os.listdir(hist_dir) if f.endswith(".log")])
    next_idx = len(existing_hist) + 1
    snap_path = os.path.join(hist_dir, f"{next_idx:02d}-{sha}-{args.tag}.log")
    with open(snap_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# OpenRR-ASI Validation Snapshot: {args.tag} ({sha})\n")
        f.write(f"# Date: {date_str}\n")
        f.write(f"# Archived Raw Runs: {runs_dir.replace(os.sep, '/')}\n\n")
        f.write("## 1. Unit & Hardware Suite (`test_asi_units.exe`)\n\n")
        f.write(unit_out.strip() + "\n\n")
        f.write("## 2. Same-Process 3-Run A/B Benchmark (`openrr_bench.exe`)\n\n")
        f.write("\n\n".join(bench_sections) + "\n")

    print(f"\n[ARCHIVED] Snapshot: {snap_path}")
    print(f"[ARCHIVED] Raw logs: {runs_dir}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
