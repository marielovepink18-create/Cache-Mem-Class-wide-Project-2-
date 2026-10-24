#!/usr/bin/env python3
"""Turn data/raw/*.csv into figures (results/figs), tables (results/tables) and results/summary.md.

  python scripts/analyze.py

Missing input files are skipped, so you can analyse a partial run.
Optional: data/raw/counters.csv (hardware-counter totals, see docs/COUNTERS.md) adds the miss-vs-runtime correlation.
"""
import json, math, sys
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

ROOT = Path(__file__).resolve().parent.parent
RAW, PROC, FIG, TAB = ROOT / "data" / "raw", ROOT / "data" / "processed", ROOT / "results" / "figs", ROOT / "results" / "tables"
for d in (PROC, FIG, TAB):
    d.mkdir(parents=True, exist_ok=True)

INFO = json.loads((ROOT / "data" / "sysinfo.json").read_text())
L1, L2, L3 = INFO["cache"]["l1d_bytes"], INFO["cache"]["l2_bytes"], INFO["cache"]["l3_bytes"]
GHZ = INFO["measured_core_ghz"]
PAL = ["#0072B2", "#D55E00", "#009E73", "#CC79A7", "#E69F00", "#56B4E9", "#555555"]
plt.rcParams.update({"figure.dpi": 130, "axes.grid": True, "grid.alpha": 0.3, "axes.spines.top": False, "axes.spines.right": False,
                     "font.size": 9, "legend.fontsize": 8, "axes.prop_cycle": matplotlib.cycler(color=PAL)})
SUMMARY = []


def say(*lines):
    SUMMARY.extend(lines)


def load(name):
    p = RAW / name
    if not p.exists() or p.stat().st_size == 0:
        return None
    df = pd.read_csv(p)
    df = df[df["tool"] != "tool"].copy()
    if "tag" in df.columns:
        df["tag"] = df["tag"].fillna("").astype(str)
    return df


def fsize(b):
    b = float(b)
    for u, s in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if b >= s:
            v = round(b / s, 2)
            return f"{v:g} {u}"
    return f"{b:g} B"


def md_table(df, fmt="{:.2f}"):
    cols = list(df.columns)
    out = ["| " + " | ".join(map(str, cols)) + " |", "|" + "---|" * len(cols)]
    for _, r in df.iterrows():
        out.append("| " + " | ".join(fmt.format(v) if isinstance(v, (float, np.floating)) else str(v) for v in r) + " |")
    return "\n".join(out)


def save_table(df, name, fmt="{:.2f}"):
    df.to_csv(TAB / f"{name}.csv", index=False)
    (TAB / f"{name}.md").write_text(md_table(df, fmt) + "\n")


def stats(df, by, col):
    g = df.groupby(by)[col]
    return pd.DataFrame({"mean": g.mean(), "std": g.std().fillna(0.0), "min": g.min(), "max": g.max(), "n": g.count()}).reset_index()


def size_axis(ax, lo=4096, hi=1 << 30):
    ticks = [2 ** k for k in range(int(math.log2(lo)), int(math.log2(hi)) + 1, 2)]
    ax.set_xscale("log", base=2)
    ax.set_xticks(ticks)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: fsize(v).replace(" ", "")))
    ax.tick_params(axis="x", labelrotation=45)
    ax.set_xlabel("working-set size (per array / buffer)")


def cache_lines(ax, text=True):
    for name, sz in (("L1d", L1), ("L2", L2), ("L3", L3)):
        if sz:
            ax.axvline(sz, color="#888", ls="--", lw=0.8)
            if text:
                ax.text(sz, 1.0, f" {name}", transform=ax.get_xaxis_transform(), va="top", fontsize=7, color="#555")


def done(fig, name):
    fig.tight_layout()
    fig.savefig(FIG / f"{name}.png")
    plt.close(fig)
    print("wrote", f"results/figs/{name}.png")


# ------------------------------------------------------------------------------------------------ zero-queue table
ZQ = {}


def zero_queue():
    d = load("lat_zeroq.csv")
    if d is None:
        return
    s = stats(d, "tag", "ns_per_step")
    order = {"L1": 0, "L2": 1, "L3": 2, "DRAM": 3}
    s["o"] = s["tag"].map(order)
    s = s.sort_values("o")
    size = d.groupby("tag")["size_bytes"].first()
    t = pd.DataFrame({"level": s["tag"], "buffer": [fsize(size[x]) for x in s["tag"]], "latency_ns": s["mean"], "std_ns": s["std"],
                      "min_ns": s["min"], "n": s["n"], f"cycles_@{GHZ:.2f}GHz": s["mean"] * GHZ})
    save_table(t, "zero_queue")
    for _, r in s.iterrows():
        ZQ[r["tag"]] = r["mean"]
    say("## Zero-queue latency (1 dependent random chain, 4 KiB pages, pinned)", "", md_table(t), "",
        f"Cycle conversion uses the measured core clock ({GHZ:.2f} GHz from a dependent-IMUL chain on the pinned core); "
        f"the clock during memory-bound phases may differ.", "")


# ------------------------------------------------------------------------------------------------ working-set sweep
def amat_model(sizes):
    """Capacity-only AMAT: random access to S bytes; a level 'holds' min(1, C/S) of the data (inclusive caps, ideal LRU)."""
    if not all(k in ZQ for k in ("L1", "L2", "L3", "DRAM")):
        return None
    lat = [ZQ["L1"], ZQ["L2"], ZQ["L3"], ZQ["DRAM"]]
    out = []
    for S in sizes:
        h1, h2, h3 = (min(1.0, c / S) if c else 0.0 for c in (L1, L2, L3))
        p = [h1, max(0, h2 - h1), max(0, h3 - h2), 1 - h3]
        out.append(sum(pi * li for pi, li in zip(p, lat)))
    return out


def ws_latency():
    d = load("lat_ws.csv")
    if d is None:
        return
    s = stats(d, "size_bytes", "ns_per_step")
    s.to_csv(PROC / "lat_ws_stats.csv", index=False)
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.errorbar(s["size_bytes"], s["mean"], yerr=s["std"], fmt="o-", ms=3, lw=1.2, capsize=2, label="measured (4 KiB pages, random chase)")
    m = amat_model(s["size_bytes"])
    if m:
        ax.plot(s["size_bytes"], m, "k:", lw=1.2, label="capacity-only AMAT model from zero-queue table")
    t = load("lat_tlb.csv")
    if t is not None:
        h = t[(t.stride == 64) & (t.huge_req == 1)]
        if len(h):
            hs = stats(h, "size_bytes", "ns_per_step")
            ax.errorbar(hs["size_bytes"], hs["mean"], yerr=hs["std"], fmt="s--", ms=3, lw=1, capsize=2, label="large/huge pages requested")
    ax.set_yscale("log")
    size_axis(ax)
    cache_lines(ax)
    ax.set_ylabel("latency per dependent load (ns)")
    ax.set_title("Working-set sweep: zero-queue latency")
    ax.legend(loc="upper left", bbox_to_anchor=(0.0, 0.9))
    done(fig, "fig_ws_latency")
    if m:
        err = np.abs(np.array(m) - s["mean"].values) / s["mean"].values
        say("## Working-set latency sweep", "", "![](figs/fig_ws_latency.png)", "",
            f"Capacity-only AMAT model vs measurement: median |error| {np.median(err)*100:.0f}%, worst {err.max()*100:.0f}% "
            f"at {fsize(s['size_bytes'].values[err.argmax()])} (model ignores TLB misses, associativity and prefetch).", "")


def bw_ws():
    d = load("bw_ws.csv")
    k = load("kernel_ws.csv")
    if d is None and k is None:
        return
    fig, ax = plt.subplots(1, 2 if k is not None else 1, figsize=(10 if k is not None else 6, 4), squeeze=False)
    if d is not None:
        s = stats(d, "size_bytes", "gbps_total")
        ax[0, 0].errorbar(s["size_bytes"], s["mean"], yerr=s["std"], fmt="o-", ms=3, capsize=2)
        ax[0, 0].set_ylabel("read bandwidth, 1 thread (GB/s)")
        ax[0, 0].set_title("bw tool: sequential 1 KiB blocks (loop overhead caps in-cache numbers)")
        size_axis(ax[0, 0], 8192)
        cache_lines(ax[0, 0])
        peak = s.sort_values("size_bytes")
        say("## Bandwidth vs working set (single thread)", "", "![](figs/fig_ws_bandwidth.png)", "",
            f"Largest-size (DRAM-resident) single-thread read bandwidth: {peak['mean'].iloc[-1]:.1f} GB/s; best in-cache: {peak['mean'].max():.1f} GB/s.", "")
    if k is not None:
        for i, kern in enumerate(("mul", "saxpy")):
            kk = k[k.kernel == kern]
            if not len(kk):
                continue
            s = stats(kk, "size_bytes", "gbps")
            ax[0, 1].errorbar(s["size_bytes"], s["mean"], yerr=s["std"], fmt="o-", ms=3, capsize=2, label=f"{kern} ({'z=x*y' if kern=='mul' else 'y=a*x+y'})")
        ax[0, 1].set_ylabel("kernel traffic (GB/s, 24 B per element)")
        ax[0, 1].set_title("compiler-vectorised kernels, unit stride")
        size_axis(ax[0, 1], 4096, 1 << 30)
        cache_lines(ax[0, 1])
        ax[0, 1].legend()
    done(fig, "fig_ws_bandwidth")


# ------------------------------------------------------------------------------------------------ patterns / granularity
def patterns():
    lp = load("lat_patterns.csv")
    bp = load("bw_patterns.csv")
    bs = load("bw_stride.csv")
    if lp is None and bp is None and bs is None:
        return
    fig, ax = plt.subplots(1, 3, figsize=(13, 4))
    if lp is not None:
        s = stats(lp, ["pattern", "stride"], "ns_per_step")
        for i, pat in enumerate(("seq", "rand")):
            q = s[s.pattern == pat]
            ax[0].bar(np.arange(len(q)) + 0.4 * i, q["mean"], 0.38, yerr=q["std"], label=pat, capsize=2)
        strides = sorted(s.stride.unique())
        ax[0].set_xticks(np.arange(len(strides)) + 0.2)
        ax[0].set_xticklabels([f"{x} B" for x in strides])
        ax[0].set_yscale("log")
        ax[0].set_ylabel("latency per dependent load (ns)")
        ax[0].set_xlabel("node stride")
        ax[0].set_title("Latency: DRAM-sized chase")
        ax[0].legend()
        save_table(s.rename(columns={"mean": "ns_mean", "std": "ns_std"}), "lat_patterns")
        seq64 = s[(s.pattern == "seq") & (s.stride == 64)]["mean"]
        rnd64 = s[(s.pattern == "rand") & (s.stride == 64)]["mean"]
        if len(seq64) and len(rnd64):
            say("## Pattern / granularity", "", "![](figs/fig_patterns.png)", "",
                f"Sequential 64 B-stride chase over a DRAM-sized buffer: {seq64.iloc[0]:.1f} ns/load vs {rnd64.iloc[0]:.1f} ns for a random chase "
                f"({rnd64.iloc[0]/seq64.iloc[0]:.0f}x). The sequential number is NOT a DRAM latency - the prefetchers hide it (benchmark pitfall).", "")
    if bp is not None:
        s = stats(bp, ["threads", "pattern", "gran"], "gbps_total")
        save_table(s.rename(columns={"mean": "gbps_mean", "std": "gbps_std"}), "bw_patterns")
        for ti, T in enumerate(sorted(s.threads.unique())):
            for pi, pat in enumerate(("seq", "rand")):
                q = s[(s.threads == T) & (s.pattern == pat)].sort_values("gran")
                ax[1].errorbar(q["gran"], q["mean"], yerr=q["std"], fmt=["o-", "s--"][pi], color=PAL[ti], capsize=2, ms=4, label=f"{pat}, {T} thr")
        ax[1].set_xscale("log", base=2)
        ax[1].set_xlabel("access granularity (contiguous bytes per block)")
        ax[1].set_ylabel("bandwidth (GB/s)")
        ax[1].set_title("Bandwidth vs granularity (reads)")
        ax[1].legend()
        mx = s.loc[s["mean"].idxmax()]
        say(f"Highest read bandwidth observed: {mx['mean']:.1f} GB/s ({int(mx.threads)} threads, {mx.pattern}, {int(mx.gran)} B granularity).", "")
    if bs is not None:
        s = stats(bs, ["threads", "stride"], "gbps_total")
        save_table(s.rename(columns={"mean": "gbps_mean", "std": "gbps_std"}), "bw_stride")
        for ti, T in enumerate(sorted(s.threads.unique())):
            q = s[s.threads == T].sort_values("stride")
            ax[2].errorbar(q["stride"], q["mean"], yerr=q["std"], fmt="o-", color=PAL[ti], capsize=2, ms=4, label=f"{T} thr")
        ax[2].set_xscale("log", base=2)
        ax[2].set_xlabel("stride between touched 64 B lines (bytes)")
        ax[2].set_ylabel("useful bandwidth (GB/s, touched bytes only)")
        ax[2].set_title("Strided reads, one line per stride")
        ax[2].legend()
        say("Strided-read table: `results/tables/bw_stride.md`. Useful bandwidth counts only the 64 B touched per stride, so a drop means the memory system "
            "is moving lines nobody uses (adjacent-line/stream prefetch) or losing prefetch coverage past the page boundary.", "")
    done(fig, "fig_patterns")


# ------------------------------------------------------------------------------------------------ read/write mix
def rw_mix():
    d = load("bw_rw.csv")
    lr = load("loaded_rw.csv")
    if d is None and lr is None:
        return
    fig, ax = plt.subplots(1, 2 if lr is not None else 1, figsize=(10 if lr is not None else 5.5, 4), squeeze=False)
    if d is not None:
        s = stats(d, ["threads", "rw", "nt"], "gbps_total")
        save_table(s.rename(columns={"mean": "gbps_mean", "std": "gbps_std"}), "bw_rw")
        rws = [100, 70, 50, 0]
        w = 0.8 / (2 * s.threads.nunique())
        i = 0
        for T in sorted(s.threads.unique()):
            for nt in (0, 1):
                q = s[(s.threads == T) & (s.nt == nt)].set_index("rw").reindex(rws)
                ax[0, 0].bar(np.arange(4) + i * w, q["mean"], w, yerr=q["std"], capsize=2, label=f"{T} thr, {'non-temporal' if nt else 'normal'} stores")
                i += 1
        ax[0, 0].set_xticks(np.arange(4) + w * (i - 1) / 2)
        ax[0, 0].set_xticklabels(["100% R", "70/30", "50/50", "100% W"])
        ax[0, 0].set_ylabel("bandwidth of bytes touched (GB/s)")
        ax[0, 0].set_title("Read/write mix (sequential 1 KiB blocks)")
        ax[0, 0].legend()
        r = s[(s.threads == s.threads.max()) & (s.nt == 0)].set_index("rw")["mean"]
        say("## Read/write mix", "", "![](figs/fig_rw_mix.png)", "",
            f"At {int(s.threads.max())} threads with normal stores: 100% read {r.get(100, float('nan')):.1f} GB/s, 70/30 {r.get(70, float('nan')):.1f}, "
            f"50/50 {r.get(50, float('nan')):.1f}, 100% write {r.get(0, float('nan')):.1f} GB/s. Normal stores also trigger a read-for-ownership, so the DRAM "
            f"bus carries up to ~2x the counted write bytes; non-temporal stores avoid that.", "")
    if lr is not None:
        s = stats(lr, "rw", "lat_ns").set_index("rw").reindex([100, 70, 50, 0])
        b = stats(lr, "rw", "gbps_load").set_index("rw").reindex([100, 70, 50, 0])
        a2 = ax[0, 1]
        a2.bar(np.arange(4), s["mean"], 0.4, yerr=s["std"], capsize=2, color=PAL[1])
        a2.set_ylabel("loaded latency of 1 chase thread (ns)", color=PAL[1])
        a3 = a2.twinx()
        a3.plot(np.arange(4), b["mean"], "o-", color=PAL[0])
        a3.set_ylim(0, float(np.nanmax(b["mean"])) * 1.2)
        a3.set_ylabel("load-thread bandwidth (GB/s)", color=PAL[0])
        a3.grid(False)
        a2.set_xticks(np.arange(4))
        a2.set_xticklabels(["100% R", "70/30", "50/50", "100% W"])
        a2.set_title("Latency and throughput at maximum intensity")
        t = pd.DataFrame({"mix_%read": [100, 70, 50, 0], "lat_ns": s["mean"].values, "lat_std": s["std"].values, "gbps_load": b["mean"].values})
        save_table(t, "loaded_rw")
    done(fig, "fig_rw_mix")


# ------------------------------------------------------------------------------------------------ intensity sweep / Little's law
def intensity():
    d = load("loaded_intensity.csv")
    if d is None:
        return
    g = d.groupby(["threads", "delay"]).agg(lat=("lat_ns", "mean"), lat_sd=("lat_ns", "std"), bw=("gbps_load", "mean"), bw_sd=("gbps_load", "std")).reset_index().fillna(0)
    g["N_lines"] = g["bw"] * g["lat"] / 64.0  # GB/s * ns = bytes in flight; /64 = cache lines
    g.to_csv(PROC / "loaded_intensity_stats.csv", index=False)
    idle = g[g.threads == 0]
    L0 = idle["lat"].iloc[0] if len(idle) else g["lat"].min()
    load_g = g[g.threads > 0]
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.2))
    for i, T in enumerate(sorted(load_g.threads.unique())):
        q = load_g[load_g.threads == T].sort_values("bw")
        ax[0].errorbar(q["bw"], q["lat"], yerr=q["lat_sd"], fmt="o-", ms=3.5, lw=1.1, capsize=2, color=PAL[i % len(PAL)], label=f"{T} load thr")
        ax[1].plot(q["bw"], q["N_lines"], "o-", ms=3.5, lw=1.1, color=PAL[i % len(PAL)], label=f"{T} load thr")
    ax[0].axhline(L0, color="#777", ls=":", lw=1)
    ax[0].text(ax[0].get_xlim()[0], L0, f" idle {L0:.0f} ns", va="bottom", fontsize=7)
    ax[0].set_xlabel("delivered load bandwidth (GB/s)")
    ax[0].set_ylabel("latency seen by 1 chase thread (ns)")
    ax[0].set_title("Throughput-latency curves (delay knob swept)")
    ax[0].legend()
    # knee on the highest-thread curve
    top = load_g[load_g.threads == load_g.threads.max()].sort_values("delay", ascending=False)
    peak = load_g["bw"].max()
    sat = top[top.bw >= 0.9 * peak].sort_values("bw").head(1)
    dbl = top[top.lat >= 2 * L0].sort_values("bw").head(1)
    for name, row, col in (("90% of peak BW", sat, PAL[2]), ("latency = 2x idle", dbl, PAL[1])):
        if len(row):
            ax[0].plot(row["bw"], row["lat"], "*", ms=14, color=col, label=name, zorder=5)
    ax[0].legend()
    Nmax = load_g["N_lines"].max()
    ax[1].set_xlabel("delivered load bandwidth (GB/s)")
    ax[1].set_ylabel("N = BW x latency / 64 B  (cache lines in flight)")
    ax[1].set_title("Little's Law: concurrency actually sustained")
    ax[1].legend()
    done(fig, "fig_intensity")
    save_table(g, "loaded_intensity")
    lines = ["## Throughput-latency (intensity) sweep", "", "![](figs/fig_intensity.png)", "",
             f"Idle (no load threads) latency of the chase thread: **{L0:.0f} ns**. Peak delivered load bandwidth: **{peak:.1f} GB/s**."]
    if len(sat):
        r = sat.iloc[0]
        lines.append(f"Saturation point (first point on the {int(load_g.threads.max())}-thread curve reaching 90% of peak): {r.bw:.1f} GB/s at {r.lat:.0f} ns "
                     f"({r.lat/L0:.2f}x idle), delay={int(r.delay)}.")
    if len(dbl):
        r = dbl.iloc[0]
        lines.append(f"Latency reaches 2x idle at {r.bw:.1f} GB/s ({r.bw/peak*100:.0f}% of peak), delay={int(r.delay)}.")
    lines.append(f"Little's Law check: N = BW x latency / 64 B reaches {Nmax:.0f} cache lines in flight at the heaviest load "
                 f"(idle chase thread alone: 1). If N stops growing while latency keeps rising, the memory system - not the cores - is the limiter.")
    lines.append("")
    say(*lines)


def chains():
    d = load("lat_chains.csv")
    if d is None:
        return
    s = d.groupby("chains").agg(ns=("ns_per_step", "mean"), sd=("ns_per_step", "std"), gbps=("gbps", "mean")).reset_index().fillna(0)
    s["N"] = s["chains"]
    fig, ax = plt.subplots(1, 2, figsize=(9.5, 3.8))
    ax[0].errorbar(s["chains"], s["gbps"], fmt="o-", ms=4)
    ax[0].set_xscale("log", base=2)
    ax[0].set_xlabel("independent chains = outstanding misses N")
    ax[0].set_ylabel("single-core bandwidth (GB/s)")
    ax[0].set_title("MLP of one core: bandwidth = N x 64 B / latency")
    ax[1].errorbar(s["chains"], s["ns"], yerr=s["sd"], fmt="o-", ms=4, capsize=2, color=PAL[1])
    ax[1].set_xscale("log", base=2)
    ax[1].set_xlabel("independent chains N")
    ax[1].set_ylabel("latency per load (ns)")
    ax[1].set_title("Latency stays flat until the core's miss buffers fill")
    done(fig, "fig_chains")
    save_table(s[["chains", "ns", "sd", "gbps"]], "lat_chains")
    pk = s["gbps"].max()
    k90 = s[s.gbps >= 0.9 * pk]["chains"].min()
    n_eff = pk * s[s.chains == k90]["ns"].iloc[0] / 64
    say("## Memory-level parallelism of one core", "", "![](figs/fig_chains.png)", "",
        f"Single-core bandwidth rises almost linearly with N (Little's Law: BW = N x 64 B / latency) and flattens at {pk:.1f} GB/s; 90% of that is reached at "
        f"N = {int(k90)} chains. Little's Law applied to the plateau gives ~{n_eff:.1f} concurrent line fills, the practical per-core miss-handling limit "
        f"(L1 fill buffers / L2 super-queue / prefetch).", "")


# ------------------------------------------------------------------------------------------------ TLB / kernels / counters
def tlb():
    d = load("lat_tlb.csv")
    kt = load("kernel_tlb.csv")
    if d is None and kt is None:
        return
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    if d is not None:
        for i, (st, hg, lbl) in enumerate(((4160, 0, "1 node/page, 4 KiB pages"), (4160, 1, "1 node/page, large pages"), (64, 1, "dense (64 B), large pages"))):
            q = d[(d.stride == st) & (d.huge_req == hg)]
            if not len(q):
                continue
            s = stats(q, "size_bytes", "ns_per_step")
            ok = q["huge_ok"].max() if hg else 0
            lbl2 = lbl + ("" if not hg else (" (granted)" if ok == 1 else " (NOT granted!)" if ok == 0 else " (granted? unverified)"))
            ax[0].errorbar(s["size_bytes"], s["mean"], yerr=s["std"], fmt="o-", ms=3, capsize=2, label=lbl2)
        lw = load("lat_ws.csv")
        if lw is not None:
            s = stats(lw, "size_bytes", "ns_per_step")
            ax[0].plot(s["size_bytes"], s["mean"], "k:", lw=1.2, label="dense (64 B), 4 KiB pages (lat_ws)")
        ax[0].set_yscale("log")
        size_axis(ax[0], 65536)
        cache_lines(ax[0])
        ax[0].set_ylabel("latency per dependent load (ns)")
        ax[0].set_title("TLB reach: latency when every load touches a new page")
        ax[0].legend()
        a = d[(d.stride == 4160) & (d.huge_req == 0)]
        b = d[(d.stride == 4160) & (d.huge_req == 1)]
        if len(a) and len(b):
            big = a["size_bytes"].max()
            la, lb = a[a.size_bytes == big]["ns_per_step"].mean(), b[b.size_bytes == big]["ns_per_step"].mean()
            say("## TLB impact", "", "![](figs/fig_tlb.png)", "",
                f"At {fsize(big)} with one node per page: {la:.0f} ns with 4 KiB pages vs {lb:.0f} ns with large pages requested "
                f"(granted: {'yes' if b['huge_ok'].max()==1 else 'NO/unverified - check Lock-pages-in-memory'}); difference {la-lb:.0f} ns/load = page-walk cost.", "")
    if kt is not None:
        kt = kt.copy()
        kt["config"] = kt["tag"].astype(str) + np.where(kt["order"] == "rand", " blk=" + kt["blk"].astype(str), "")
        s = stats(kt, ["config", "huge_req"], "ns_per_elem")
        cfgs = list(dict.fromkeys(kt["config"]))
        for i, hg in enumerate((0, 1)):
            q = s[s.huge_req == hg].set_index("config").reindex(cfgs)
            ax[1].bar(np.arange(len(cfgs)) + 0.4 * i, q["mean"], 0.38, yerr=q["std"], capsize=2, label="large pages" if hg else "4 KiB pages")
        ax[1].set_xticks(np.arange(len(cfgs)) + 0.2)
        ax[1].set_xticklabels(cfgs, rotation=20, ha="right")
        ax[1].set_ylabel("ns per element (z=x*y)")
        ax[1].set_title("Kernel: random page order, DRAM-sized arrays")
        ax[1].legend()
        save_table(s.rename(columns={"mean": "ns_mean", "std": "ns_std"}), "kernel_tlb")
    done(fig, "fig_tlb")


def kernels_cache():
    d = load("kernel_cache.csv")
    if d is None:
        return
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    b = d[d.tag == "blk_sweep"].copy()
    b["lbl"] = np.where(b["order"] == "seq", "seq", b["blk"].astype(int).map(lambda v: fsize(v).replace(" ", "")))
    s = stats(b, "lbl", "ns_per_elem")
    order = ["seq"] + [fsize(v).replace(" ", "") for v in sorted(b[b["order"] == "rand"]["blk"].unique())]
    s = s.set_index("lbl").reindex(order)
    ax[0].bar(range(len(order)), s["mean"], yerr=s["std"], capsize=2)
    ax[0].set_xticks(range(len(order)))
    ax[0].set_xticklabels(order, rotation=45)
    ax[0].set_xlabel("contiguous block visited (random block order)")
    ax[0].set_ylabel("ns per element")
    ax[0].set_title(f"Spatial locality: z=x*y, {fsize(b['size_bytes'].max())} per array")
    f = d[d.tag == "fp_rand64"]
    if len(f):
        fs = stats(f, "size_bytes", "ns_per_elem")
        ax[1].errorbar(fs["size_bytes"], fs["mean"], yerr=fs["std"], fmt="o-", ms=3, capsize=2)
        ax[1].set_yscale("log")
        size_axis(ax[1])
        cache_lines(ax[1])
        ax[1].set_ylabel("ns per element")
        ax[1].set_title("Footprint sweep, random 64 B-line order (3 arrays total = 3x)")
    done(fig, "fig_kernel_cache")
    save_table(stats(d, ["tag", "order", "blk", "size_bytes"], "ns_per_elem"), "kernel_cache")
    seq, rnd = s.loc["seq", "mean"], s.iloc[1]["mean"] if len(s) > 1 else float("nan")
    say("## Cache-miss impact (kernel)", "", "![](figs/fig_kernel_cache.png)", "",
        f"Same arithmetic, different visit order: sequential {seq:.2f} ns/element vs {rnd:.2f} ns/element with random {order[1]} blocks ({rnd/seq:.1f}x).", "")


def counters():
    c = RAW / "counters.csv"
    if not c.exists():
        say("## Hardware counters", "", "`data/raw/counters.csv` not found - see docs/COUNTERS.md. Without counters the cache/TLB conclusions rest on latency "
            "transitions only, which is weaker evidence.", "")
        return
    c = pd.read_csv(c)
    c = c.dropna(axis=1, how="all")
    run = pd.concat([x for x in (load("kernel_cache.csv"), load("kernel_tlb.csv")) if x is not None], ignore_index=True) if (
        load("kernel_cache.csv") is not None or load("kernel_tlb.csv") is not None) else None
    miss_cols = [x for x in c.columns if x.endswith("_miss") or x.endswith("_misses")]
    if "elements" not in c.columns or not miss_cols:
        say("## Hardware counters", "", "counters.csv needs `elements` and at least one *_miss column.", "")
        return
    for m in miss_cols:
        c[m + "_per_elem"] = c[m] / c["elements"]
    if "runtime_ns_per_elem" not in c.columns and run is not None:
        key = ["size_bytes", "order", "blk", "huge_req"]
        c = c.merge(run.groupby(key)["ns_per_elem"].mean().reset_index().rename(columns={"ns_per_elem": "runtime_ns_per_elem"}), on=key, how="left")
    if "runtime_ns_per_elem" not in c.columns:
        c["runtime_ns_per_elem"] = c["cycles"] / c["elements"] / GHZ if "cycles" in c.columns else np.nan
    fig, ax = plt.subplots(1, len(miss_cols), figsize=(4.2 * len(miss_cols), 3.6), squeeze=False)
    out = ["## Hardware counters vs runtime", "", "![](figs/fig_counters.png)", "", "| counter | Pearson r | Spearman rho | n |", "|---|---|---|---|"]
    for i, m in enumerate(miss_cols):
        x, y = c[m + "_per_elem"], c["runtime_ns_per_elem"]
        ax[0, i].scatter(x, y, s=22)
        ax[0, i].set_xlabel(f"{m} per element")
        ax[0, i].set_ylabel("ns per element")
        ok = x.notna() & y.notna()
        if ok.sum() >= 3:
            out.append(f"| {m} | {np.corrcoef(x[ok], y[ok])[0,1]:.2f} | {x[ok].rank().corr(y[ok].rank()):.2f} | {int(ok.sum())} |")
    done(fig, "fig_counters")
    c.to_csv(PROC / "counters_processed.csv", index=False)
    say(*out, "")


# ------------------------------------------------------------------------------------------------ pitfalls
def pitfalls():
    p = load("pitfall_pin.csv")
    pb = load("pitfall_pin_bw.csv")
    if pb is not None:
        p = pb if p is None else pd.concat([p, pb], ignore_index=True)
    f = load("pitfall_freq.csv")
    lp = load("lat_patterns.csv")
    if p is None and f is None:
        return
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    if p is not None:
        tags = list(dict.fromkeys(p["tag"]))
        lat = p[p["tool"] == "lat"]
        bw = p[p["tool"] == "bw"]
        rows = []
        for t in tags:
            q = p[p.tag == t]
            v = q["ns_per_step"] if q["tool"].iloc[0] == "lat" else q["gbps_total"]
            rows.append({"config": t, "unit": "ns" if q["tool"].iloc[0] == "lat" else "GB/s", "mean": v.mean(), "std": v.std(), "cv_%": 100 * v.std() / v.mean(),
                         "min": v.min(), "max": v.max(), "n": len(v)})
        t = pd.DataFrame(rows)
        save_table(t, "pitfall_pin")
        for i, tg in enumerate([x for x in tags if x.startswith(("L2", "L3", "DRAM"))]):
            q = lat[lat.tag == tg]["ns_per_step"]
            ax[0].plot(np.full(len(q), i) + np.random.RandomState(i).uniform(-0.12, 0.12, len(q)), q, "o", ms=3, color=PAL[1 if "unpinned" in tg else 0], alpha=0.7)
        lbls = [x for x in tags if x.startswith(("L2", "L3", "DRAM"))]
        ax[0].set_xticks(range(len(lbls)))
        ax[0].set_xticklabels(lbls, rotation=30, ha="right")
        ax[0].set_yscale("log")
        ax[0].set_ylabel("ns per load (each dot = one 100 ms window)")
        ax[0].set_title("Pitfall: pinned (blue) vs unpinned (orange)")
        w = t[t.config.str.contains("unpinned")].merge(t[t.config.str.contains("_pinned")], how="cross", suffixes=("_u", "_p"))
        w = w[w.config_u.str.replace("_unpinned", "") == w.config_p.str.replace("_pinned", "")]
        say("## Pitfall A: pinning", "", "![](figs/fig_pitfalls.png)", "", md_table(t), "")
        for _, r in w.iterrows():
            say(f"- {r.config_u.replace('_unpinned','')}: coefficient of variation {r['cv_%_p']:.1f}% pinned vs {r['cv_%_u']:.1f}% unpinned "
                f"(mean {r.mean_p:.2f} vs {r.mean_u:.2f} {r.unit_p}).")
        say("")
    if f is not None:
        f = f.copy()
        f["policy"] = f["tag"].str.split(":").str[0]
        f["level"] = f["tag"].str.split(":").str[1]
        s = f.groupby(["policy", "level"]).agg(ns=("ns_per_step", "mean"), sd=("ns_per_step", "std")).reset_index()
        s["cycles"] = s["ns"] * GHZ
        save_table(s, "pitfall_freq")
        pol = list(dict.fromkeys(s["policy"]))
        lv = [x for x in ("L1", "L2", "DRAM") if x in set(s["level"])]
        for i, pl in enumerate(pol):
            q = s[s.policy == pl].set_index("level").reindex(lv)
            ax[1].bar(np.arange(len(lv)) + 0.8 / len(pol) * i, q["ns"], 0.8 / len(pol), yerr=q["sd"], capsize=2, label=pl)
        ax[1].set_xticks(np.arange(len(lv)) + 0.4 - 0.4 / len(pol))
        ax[1].set_xticklabels(lv)
        ax[1].set_yscale("log")
        ax[1].set_ylabel("ns per load")
        ax[1].set_title("Pitfall: power/frequency policy")
        ax[1].legend()
        say("## Pitfall B: frequency policy", "", md_table(s), "",
            "Expectation to check against the table: cache latencies in ns scale with core clock while DRAM latency (memory/uncore clock) mostly does not, so converting ns to cycles with a stale frequency is wrong.", "")
    done(fig, "fig_pitfalls")


def main():
    zero_queue()
    ws_latency()
    bw_ws()
    patterns()
    rw_mix()
    intensity()
    chains()
    tlb()
    kernels_cache()
    counters()
    pitfalls()
    head = ["# Auto-generated results summary", "",
            f"CPU: {INFO['cpu_model']} | {INFO['physical_cores']} cores / {INFO['logical_cpus']} threads (SMT {'on' if INFO['smt_enabled'] else 'off'}) | "
            f"L1d {fsize(L1)}, L2 {fsize(L2)}, L3 {fsize(L3)} | measured {GHZ:.2f} GHz", "",
            "All numbers below are generated from data/raw by scripts/analyze.py. Interpretation belongs in REPORT.md.", ""]
    (ROOT / "results" / "summary.md").write_text("\n".join(head + SUMMARY) + "\n")
    print("wrote results/summary.md")


if __name__ == "__main__":
    main()
