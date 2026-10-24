#!/usr/bin/env python3
"""Run every experiment and append raw CSV rows to data/raw/*.csv.

  python scripts/run_all.py                 # full run (~25-40 min on a desktop CPU)
  python scripts/run_all.py --quick         # coarse sweeps, 1 seed, 3 reps (~5-8 min) - use first to check setup
  python scripts/run_all.py --only lat_ws,loaded
  python scripts/run_all.py --only pitfall_freq --tag powersaver     # repeat after switching the Windows power plan
  python scripts/run_all.py --list

Each experiment deletes and rewrites its own output files, so re-running one experiment is safe.
Close other programs, plug in AC power and select the High performance power plan before starting.
"""
import argparse, json, math, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN, RAW = ROOT / "bin", ROOT / "data" / "raw"
EXE = ".exe" if os.name == "nt" else ""
KiB, MiB, GiB = 1 << 10, 1 << 20, 1 << 30


class Ctx:
    pass


C = Ctx()
_count = [0]
_errors = []


def reset(*names):
    for n in names:
        p = RAW / n
        if p.exists():
            p.unlink()


def run(tool, out, **kw):
    """Run one tool invocation and append its CSV rows to data/raw/<out>."""
    path = RAW / out
    cmd = [str(BIN / (tool + EXE))] + [f"{k}={v}" for k, v in kw.items()]
    if not path.exists() or path.stat().st_size == 0:
        cmd.append("-H")
    _count[0] += 1
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    except subprocess.TimeoutExpired:
        _errors.append(f"TIMEOUT {' '.join(cmd)}")
        print(f"   !! timeout: {' '.join(cmd)}")
        return False
    if r.returncode != 0 or not r.stdout.strip():
        msg = f"FAIL rc={r.returncode} {' '.join(cmd)} :: {r.stderr.strip()[:200]}"
        _errors.append(msg)
        print("   !!", msg)
        return False
    with open(path, "a", newline="") as f:
        f.write(r.stdout)
    print(f"   [{_count[0]:4d}] {tool} {' '.join(f'{k}={v}' for k, v in kw.items() if k not in ('reps','ms','tag'))}  ({time.time()-t0:.1f}s)", flush=True)
    return True


def sweep(lo, hi, per_octave=2):
    f = 2 ** (1 / per_octave)
    out, s = [], float(lo)
    while s <= hi * 1.0001:
        v = int(round(s / 4096)) * 4096 if s >= 4096 else int(s)
        if not out or v != out[-1]:
            out.append(v)
        s *= f
    return out


def counts(n):
    base = [1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32]
    c = [x for x in base if x <= n]
    if n not in c:
        c.append(n)
    return c


def cpu_list(cpus):
    return ",".join(str(c) for c in cpus)


# ------------------------------------------------------------------------------------------------ experiments
def exp_lat_ws():
    """Working-set sweep of zero-queue (single dependent chain) latency, 4 KiB pages, random pattern."""
    reset("lat_ws.csv")
    for seed in C.seeds:
        for s in sweep(4 * KiB, C.dram, C.per_octave):
            run("lat", "lat_ws.csv", size=s, stride=64, pattern="rand", chains=1, cpu=C.cpu1, reps=C.reps, ms=C.ms, seed=seed)


def exp_zero_queue():
    """Per-level zero-queue latency table points: well inside L1 / L2 / L3 and in DRAM (tag = level)."""
    reset("lat_zeroq.csv")
    pts = [("L1", C.l1 // 2), ("L2", C.l2 // 2), ("L3", C.l3 // 2 if C.l3 else 8 * MiB), ("DRAM", C.dram)]
    for lvl, s in pts:
        s = max(4096, (s // 4096) * 4096)
        for seed in C.seeds:
            run("lat", "lat_zeroq.csv", size=s, stride=64, pattern="rand", chains=1, cpu=C.cpu1, reps=C.reps * 2, ms=C.ms, seed=seed, tag=lvl)


def exp_patterns():
    """Sequential vs random, stride/granularity: latency (chase) and bandwidth (streams)."""
    reset("lat_patterns.csv", "bw_patterns.csv", "bw_stride.csv")
    for pat in ("seq", "rand"):
        for stride in (64, 256, 1024):
            run("lat", "lat_patterns.csv", size=C.dram, stride=stride, pattern=pat, chains=1, cpu=C.cpu1, reps=C.reps, ms=C.ms)
    for T in sorted({1, C.tmax}):
        cp = cpu_list(C.cpus_all[:T])
        for pat in ("seq", "rand"):
            for gran in (64, 128, 256, 512, 1024, 4096):
                run("bw", "bw_patterns.csv", size=C.bwsize(T), threads=T, cpus=cp, pattern=pat, gran=gran, stride=gran, rw=100,
                    reps=C.reps, ms=C.msbw)
        for stride in (64, 128, 256, 512, 1024, 4096):
            run("bw", "bw_stride.csv", size=C.bwsize(T), threads=T, cpus=cp, pattern="seq", gran=64, stride=stride, rw=100,
                reps=C.reps, ms=C.msbw)


def exp_rw_mix():
    """Read/write mixes with normal (write-allocate) and non-temporal stores."""
    reset("bw_rw.csv")
    for T in sorted({1, C.tmax}):
        cp = cpu_list(C.cpus_all[:T])
        for rw in (100, 70, 50, 0):
            for nt in ((0, 1) if rw < 100 else (0,)):
                run("bw", "bw_rw.csv", size=C.bwsize(T), threads=T, cpus=cp, pattern="seq", gran=1024, stride=1024, rw=rw, nt=nt,
                    reps=C.reps, ms=C.msbw)


def exp_bw_ws():
    """Single-thread read bandwidth vs working-set size (L1 .. DRAM)."""
    reset("bw_ws.csv")
    for s in sweep(8 * KiB, C.dram, C.per_octave):
        run("bw", "bw_ws.csv", size=s, threads=1, cpus=C.cpu1, pattern="seq", gran=1024, stride=1024, rw=100, reps=C.reps, ms=C.msbw)


def exp_loaded():
    """Throughput-latency (intensity) sweep + read/write mix under load."""
    reset("loaded_intensity.csv", "loaded_rw.csv")
    nload = C.nload
    delays = [0, 4, 16, 64, 256, 1024, 8192, 65536] if C.quick else [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 16384, 65536]
    common = dict(latsize=C.dram, size=C.dram, latcpu=C.lat_cpu, reps=C.reps, ms=C.msbw)
    run("loaded", "loaded_intensity.csv", threads=0, delay=0, rw=100, pattern="seq", **common)  # idle (zero-load) reference
    tl = [1, nload] if C.quick else counts(nload)
    for T in sorted(set(tl)):
        for d in delays:
            run("loaded", "loaded_intensity.csv", threads=T, cpus=cpu_list(C.load_cpus[:T]), delay=d, rw=100, pattern="seq", **common)
    for rw in (100, 70, 50, 0):
        run("loaded", "loaded_rw.csv", threads=nload, cpus=cpu_list(C.load_cpus[:nload]), delay=0, rw=rw, pattern="seq", **common)


def exp_chains():
    """Memory-level parallelism of one core: K independent chains = K outstanding misses (Little's Law)."""
    reset("lat_chains.csv")
    ks = [1, 2, 4, 8, 12, 16, 32] if C.quick else [1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32, 48, 64]
    for k in ks:
        run("lat", "lat_chains.csv", size=C.dram, stride=64, pattern="rand", chains=k, cpu=C.cpu1, reps=C.reps, ms=C.ms)


def exp_tlb():
    """TLB reach: one node per page (stride 4160 = page + one line to avoid cache-set aliasing), 4 KiB vs large pages."""
    reset("lat_tlb.csv", "kernel_tlb.csv")
    for huge in (0, 1):
        for s in sweep(64 * KiB, C.dram, C.per_octave):
            run("lat", "lat_tlb.csv", size=s, stride=4160, pattern="rand", chains=1, huge=huge, cpu=C.cpu1, reps=C.reps, ms=C.ms)
    for s in sweep(64 * KiB, C.dram, C.per_octave):  # dense (stride 64) with large pages for comparison with lat_ws
        run("lat", "lat_tlb.csv", size=s, stride=64, pattern="rand", chains=1, huge=1, cpu=C.cpu1, reps=C.reps, ms=C.ms)
    ksz = C.dram // 2
    for huge in (0, 1):
        run("kernel", "kernel_tlb.csv", size=ksz, kernel="mul", order="seq", blk=0, huge=huge, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms, tag="seq")
        for blk in (4096, 65536):
            run("kernel", "kernel_tlb.csv", size=ksz, kernel="mul", order="rand", blk=blk, huge=huge, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms,
                tag="pagerand")


def exp_kernel_ws():
    """Kernel runtime / cycles per element vs footprint (unit stride)."""
    reset("kernel_ws.csv")
    for kern in ("mul", "saxpy"):
        for s in sweep(4 * KiB, C.dram // 2, C.per_octave):
            run("kernel", "kernel_ws.csv", size=s, kernel=kern, order="seq", blk=0, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms)


def exp_kernel_cache():
    """Vary spatial locality (block size of a random visit order) and footprint; used with counters."""
    reset("kernel_cache.csv")
    ksz = C.dram // 2
    run("kernel", "kernel_cache.csv", size=ksz, kernel="mul", order="seq", blk=0, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms, tag="blk_sweep")
    for blk in (64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144):
        run("kernel", "kernel_cache.csv", size=ksz, kernel="mul", order="rand", blk=blk, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms, tag="blk_sweep")
    for s in sweep(4 * KiB, ksz, C.per_octave):  # footprint sweep with random line order
        run("kernel", "kernel_cache.csv", size=s, kernel="mul", order="rand", blk=64, cpu=C.cpu1, ghz=C.ghz, reps=C.reps, ms=C.ms, tag="fp_rand64")


def exp_pitfall_pin():
    """Pitfall: same benchmark pinned vs not pinned (OS migrates the thread; many repetitions expose the spread)."""
    reset("pitfall_pin.csv", "pitfall_pin_bw.csv")
    reps = max(C.reps * 3, 10)
    for lbl, s in (("L2", C.l2 // 2), ("L3", C.l3 // 2 if C.l3 else 8 * MiB), ("DRAM", C.dram)):
        s = max(4096, (s // 4096) * 4096)
        for cpu in (C.cpu1, -1):
            run("lat", "pitfall_pin.csv", size=s, stride=64, pattern="rand", chains=1, cpu=cpu, reps=reps, ms=C.ms, tag=f"{lbl}_{'pinned' if cpu >= 0 else 'unpinned'}")
    for cpu in (C.cpu1, -1):
        run("bw", "pitfall_pin_bw.csv", size=C.dram, threads=1, cpus=cpu if cpu >= 0 else "", pattern="seq", gran=1024, stride=1024, rw=100,
            reps=reps, ms=C.msbw, tag=f"BW_{'pinned' if cpu >= 0 else 'unpinned'}")


def exp_pitfall_freq():
    """Pitfall: CPU frequency policy. Run once per policy with --tag (e.g. perf, powersaver); rows accumulate."""
    path = "pitfall_freq.csv"
    tag = C.tag or "default"
    for fn in (path, "pitfall_freq_kernel.csv"):  # keep rows of other tags, replace this tag's rows
        fp = RAW / fn
        if fp.exists():
            lines = fp.read_text().splitlines()
            keep = [l for l in lines if l.startswith("tool,") or l.split(",")[1].split(":")[0] != tag]
            fp.write_text("\n".join(keep) + "\n")
    for lbl, s in (("L1", C.l1 // 2), ("L2", C.l2 // 2), ("DRAM", C.dram)):
        s = max(4096, (s // 4096) * 4096)
        run("lat", path, size=s, stride=64, pattern="rand", chains=1, cpu=C.cpu1, reps=C.reps * 2, ms=C.ms, tag=f"{tag}:{lbl}")
    run("kernel", "pitfall_freq_kernel.csv", size=max(4096, C.l1 // 2), kernel="mul", order="seq", blk=0, cpu=C.cpu1, ghz=C.ghz, reps=C.reps * 2, ms=C.ms, tag=tag)


EXPERIMENTS = {
    "lat_ws": exp_lat_ws, "zero_queue": exp_zero_queue, "patterns": exp_patterns, "rw_mix": exp_rw_mix, "bw_ws": exp_bw_ws,
    "loaded": exp_loaded, "chains": exp_chains, "tlb": exp_tlb, "kernel_ws": exp_kernel_ws, "kernel_cache": exp_kernel_cache,
    "pitfall_pin": exp_pitfall_pin, "pitfall_freq": exp_pitfall_freq,
}
DEFAULT = [k for k in EXPERIMENTS if k != "pitfall_freq"]  # pitfall_freq needs manual power-plan changes (see README)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--tag", default="")
    ap.add_argument("--cpu", type=int, default=None, help="logical CPU for single-thread tests (default: 2nd physical core)")
    ap.add_argument("--cores", type=int, default=0, help="use only the first N physical cores (e.g. P-cores of a hybrid CPU)")
    ap.add_argument("--max-mem-gb", type=float, default=4.0, help="cap on total benchmark memory (limits thread counts)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        print("\n".join(EXPERIMENTS))
        return
    si = ROOT / "data" / "sysinfo.json"
    if not si.exists():
        print("sysinfo.json missing -> running scripts/sysinfo.py")
        subprocess.run([sys.executable, str(ROOT / "scripts" / "sysinfo.py")], check=True)
    info = json.loads(si.read_text())
    RAW.mkdir(parents=True, exist_ok=True)

    phys = info["physical_cpus"][: a.cores] if a.cores else info["physical_cpus"]
    C.quick, C.tag = a.quick, a.tag
    C.l1, C.l2, C.l3 = info["cache"]["l1d_bytes"], info["cache"]["l2_bytes"], info["cache"]["l3_bytes"]
    C.ghz = round(info["measured_core_ghz"], 3)
    C.cpu1 = a.cpu if a.cpu is not None else (phys[1] if len(phys) > 1 else phys[0])
    C.lat_cpu = phys[0] if len(phys) > 1 and C.cpu1 != phys[0] else phys[-1]
    C.cpus_all = [C.cpu1] + [c for c in phys if c != C.cpu1]
    C.load_cpus = [c for c in phys if c != C.lat_cpu]
    if not C.load_cpus:
        C.load_cpus = [c for c in range(info["logical_cpus"]) if c != C.lat_cpu]
    dram = 1 << max(28, math.ceil(math.log2(max(8 * C.l3, 1))))  # >= 8x LLC, 256 MiB..1 GiB
    C.dram = min(dram, GiB)
    maxmem = int(a.max_mem_gb * GiB)
    C.tmax = max(1, min(len(C.cpus_all), maxmem // C.dram))
    C.nload = max(1, min(len(C.load_cpus), maxmem // C.dram - 1))
    # multi-thread bandwidth tests give each thread dram/2 (still >> LLC in total once >=2 threads); single thread gets full dram
    C.bwsize = lambda T: C.dram if T == 1 else max(64 * MiB, min(C.dram, maxmem // T))
    C.reps, C.ms, C.msbw = (3, 60, 200) if a.quick else (5, 100, 300)
    C.seeds = [1] if a.quick else [1, 2]
    C.per_octave = 1 if a.quick else 2

    names = [x for x in a.only.split(",") if x] or DEFAULT
    for n in names:
        if n not in EXPERIMENTS:
            sys.exit(f"unknown experiment {n!r}; use --list")
    print(f"CPU single-thread pin: {C.cpu1} | latency-thread cpu: {C.lat_cpu} | load cpus: {C.load_cpus[:C.nload]} | DRAM buffer: {C.dram // MiB} MiB"
          f" | threads: tmax={C.tmax} nload={C.nload} | measured {C.ghz} GHz")
    if C.l3 and C.dram < 4 * C.l3:
        print("WARNING: DRAM-sized buffer is < 4x LLC; DRAM numbers may be partly cache hits.")
    t0 = time.time()
    for n in names:
        print(f"\n=== {n}: {EXPERIMENTS[n].__doc__.strip().splitlines()[0]}")
        EXPERIMENTS[n]()
    print(f"\nDone: {_count[0]} tool invocations in {(time.time()-t0)/60:.1f} min. {len(_errors)} failures.")
    for e in _errors:
        print("  ", e)
    (ROOT / "data" / "run_meta.json").write_text(json.dumps({
        "finished": time.strftime("%Y-%m-%d %H:%M:%S"), "quick": a.quick, "experiments": names, "cpu_single": C.cpu1,
        "cpu_latency_thread": C.lat_cpu, "load_cpus": C.load_cpus[:C.nload], "dram_bytes": C.dram, "reps": C.reps, "ms": C.ms,
        "ms_bw": C.msbw, "seeds": C.seeds, "failures": _errors}, indent=2))


if __name__ == "__main__":
    main()
