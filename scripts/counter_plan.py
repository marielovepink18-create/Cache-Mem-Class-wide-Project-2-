#!/usr/bin/env python3
"""Prepare the hardware-counter part of the project.

  python scripts/counter_plan.py

1. Runs 9 kernel configurations UN-profiled for a fixed number of passes (~4 s each) and records runtime + element count.
2. Writes data/raw/counters.csv with those columns filled in and the counter columns EMPTY.
3. Prints the exact command line for every row. Run each one under your counter tool (VTune / AMD uProf / perf),
   read cycles, instructions, L1D misses, L2 misses, LLC misses, DTLB misses, and type them into counters.csv.
   scripts/analyze.py then computes misses per element and correlates them with runtime (docs/COUNTERS.md).
"""
import json, math, os, subprocess, sys
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
info = json.loads((ROOT / "data" / "sysinfo.json").read_text())
l1, l2, l3 = (info["cache"][k] for k in ("l1d_bytes", "l2_bytes", "l3_bytes"))
phys = info["physical_cpus"]
cpu = phys[1] if len(phys) > 1 else phys[0]
run_meta = ROOT / "data" / "run_meta.json"
dram = json.loads(run_meta.read_text())["dram_bytes"] if run_meta.exists() else 1 << 29
ksz = dram // 2


def r4k(x):
    return max(4096, int(x) // 4096 * 4096)


# label, size, order, blk(0=size), huge
CONFIGS = [
    ("L1-resident seq", r4k(l1 // 4), "seq", 0, 0),
    ("L2-resident seq", r4k(l2 // 4), "seq", 0, 0),
    ("L3-resident seq", r4k(l3 // 8 if l3 else 1 << 22), "seq", 0, 0),
    ("DRAM seq", ksz, "seq", 0, 0),
    ("DRAM seq large pages", ksz, "seq", 0, 1),
    ("DRAM rand 4KiB blocks", ksz, "rand", 4096, 0),
    ("DRAM rand 4KiB blocks large pages", ksz, "rand", 4096, 1),
    ("DRAM rand 64B blocks", ksz, "rand", 64, 0),
    ("DRAM rand 64B blocks large pages", ksz, "rand", 64, 1),
]


def tool(args):
    r = subprocess.run([str(ROOT / "bin" / ("kernel" + EXE))] + args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr)
    return r.stdout


rows = []
for label, size, order, blk, huge in CONFIGS:
    base = [f"size={size}", "kernel=mul", f"order={order}", f"blk={blk}", f"huge={huge}", f"cpu={cpu}", f"ghz={info['measured_core_ghz']}"]
    out = tool(base + ["reps=1", "ms=300"]).strip().splitlines()  # 1 timed window to calibrate
    ns = float(out[-1].split(",")[10])  # ns_per_elem column
    ne = size // 8
    passes = max(2, int(4e9 / (ns * ne)))
    out = tool(base + [f"passes={passes}", "reps=1", "-H"]).strip().splitlines()
    f = out[-1].split(",")
    real_blk = int(f[5])
    rows.append({"label": label, "size_bytes": int(f[2]), "order": order, "blk": real_blk, "huge_req": huge, "passes": passes,
                 "elements": int(float(f[13])), "runtime_ns_per_elem": float(f[10]),
                 "cycles": "", "instructions": "", "l1_miss": "", "l2_miss": "", "llc_miss": "", "dtlb_miss": "", "stlb_miss": "",
                 "command": f"bin{os.sep}kernel{EXE} size={size} kernel=mul order={order} blk={blk} huge={huge} cpu={cpu} passes={passes} reps=1"})
    print(f"{label:38s} {ns:7.3f} ns/elem  passes={passes}")

df = pd.DataFrame(rows)
(ROOT / "data" / "raw").mkdir(parents=True, exist_ok=True)
df.drop(columns=["command"]).to_csv(ROOT / "data" / "raw" / "counters.csv", index=False)
print("\nWrote data/raw/counters.csv (counter columns empty). Profile each command below and fill the columns in:\n")
for _, r in df.iterrows():
    print(f"[{r['label']}]\n    {r['command']}")
print("\nTotals include a short init + one untimed pass (<2% of the work). Counters must be TOTALS for the whole process, not per-second rates.")
