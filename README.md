**This package has no .exe files. Build them first with `build.bat` (see `docs/BUILD_WITH_CLION.md`), then use `run_quick.bat`.**

# Project 2 - Cache & Memory Performance Profiling (ECSE 4320/6320)

Self-contained C benchmarks + Python drivers that measure zero-queue latency, bandwidth, the throughput-latency knee, working-set
transitions, cache-miss and TLB-miss effects on **Windows (MinGW-w64 GCC)** or Linux. Everything the rubric asks for is produced by
four commands; the interpretation (and your own hypotheses, AI-use note, and anomalies) goes in `REPORT.md`.

```
python scripts/build.py        # compile the five tools into bin/
python scripts/sysinfo.py      # CPU, caches, SMT, clock, OS, power plan -> data/sysinfo.json
python scripts/run_all.py      # all experiments -> data/raw/*.csv   (use --quick first)
python scripts/analyze.py      # figures, tables, results/summary.md
```

**Cannot install anything? Use `docs/NO_INSTALL.md`** - prebuilt `bin\*.exe` plus `run_quick.bat` / `run_full.bat`, no compiler or Python needed on the measured PC.
First-time Windows setup if you can install a compiler (power plan, large pages): `docs/WINDOWS_SETUP.md`.
Hardware counters (cache/TLB-miss correlation): **`docs/COUNTERS.md`**.

## Read this before you run anything

1. Write down your predictions in `HYPOTHESES.md` and commit it **before** the full run. The rubric requires a pre-experiment hypothesis.
2. Run `python scripts/run_all.py --quick` once (about 5-10 min) to check the setup and look at the plots.
3. Fix whatever looks wrong (see "Sanity checks" below), then do the full run on AC power, High-performance power plan, other apps closed.
4. Do the manual pitfall run (frequency policy) and the counter runs; they are not part of the default sweep.
5. Fill in `REPORT.md`. The AI-use note must describe what **you** did, so only write what is true.

## What is measured

| Rubric item | Experiment (`--only name`) | Tool | Raw file(s) in `data/raw/` | Figure |
|---|---|---|---|---|
| Zero-queue latency per level | `zero_queue` | `lat` | `lat_zeroq.csv` | table `results/tables/zero_queue.md` |
| Working-set sweep L1 -> DRAM | `lat_ws`, `bw_ws`, `kernel_ws` | `lat`, `bw`, `kernel` | `lat_ws.csv`, `bw_ws.csv`, `kernel_ws.csv` | `fig_ws_latency`, `fig_ws_bandwidth` |
| Sequential vs random, stride/granularity | `patterns` | `lat`, `bw` | `lat_patterns.csv`, `bw_patterns.csv`, `bw_stride.csv` | `fig_patterns` |
| Read/write mix (100R, 70/30, 50/50, 100W) | `rw_mix`, `loaded` | `bw`, `loaded` | `bw_rw.csv`, `loaded_rw.csv` | `fig_rw_mix` |
| Intensity sweep, knee, Little's Law | `loaded`, `chains` | `loaded`, `lat` | `loaded_intensity.csv`, `lat_chains.csv` | `fig_intensity`, `fig_chains` |
| Cache-miss impact | `kernel_cache` (+ counters) | `kernel` | `kernel_cache.csv`, `counters.csv` | `fig_kernel_cache`, `fig_counters` |
| TLB-miss impact, large pages | `tlb` | `lat`, `kernel` | `lat_tlb.csv`, `kernel_tlb.csv` | `fig_tlb` |
| Benchmark pitfalls | `pitfall_pin`, `pitfall_freq`, seq-vs-rand chase | `lat`, `bw` | `pitfall_pin*.csv`, `pitfall_freq*.csv`, `lat_patterns.csv` | `fig_pitfalls` |

Full run: about 25-40 minutes on a typical desktop (dominated by DRAM-sized buffers). `--quick` uses coarser sweeps, one seed, 3 reps.
Useful flags: `--only a,b`, `--cpu N` (core for single-thread tests), `--cores N` (use only the first N physical cores, e.g. the P-cores of a hybrid CPU),
`--max-mem-gb G` (cap on total benchmark memory, default 4), `--tag NAME` (for `pitfall_freq`).

## How the tools measure (so you can defend it)

* **`lat`** - pointer chasing. Nodes (one per `stride` bytes) form a random cycle (`pattern=rand`), so every load depends on the previous one and
  prefetchers cannot guess the next address. With `chains=1` this is the zero-queue latency; `chains=K` runs K independent cycles in lock-step, i.e. K
  outstanding misses (used for the Little's-Law / memory-level-parallelism plot). The buffer is written before timing so physical pages exist.
  Random chases over 4 KiB pages include TLB misses once the footprint exceeds TLB reach - that is part of the "DRAM latency" number unless large pages are used.
  `stride=4160` (page + 1 line) gives one node per page without all nodes mapping to the same cache set.
* **`bw`** - each thread streams its own private buffer in blocks of `gran` bytes spaced `stride` bytes apart; sequential or random block order
  (random uses an on-the-fly 64-bit LCG, not an index array, so the benchmark adds no traffic of its own). Per-block read/write choice implements the
  mixes. Reported GB/s (1 GB = 10^9 B) counts only bytes the kernel touches. Normal stores also cause a read-for-ownership on the bus; `nt=1` uses
  non-temporal stores to show the difference. Threads are pinned, run at high priority, and measured over fixed windows with one discarded warm-up window.
* **`loaded`** - MLC-style loaded latency: one pinned thread does a random pointer chase (1 outstanding miss) while N pinned threads stream through
  private buffers. `delay` inserts ~`delay` core cycles of busy loop after every 64 B line, so `delay=0` is maximum intensity and large `delay` is almost idle.
  Reported: chase latency and total load bandwidth; Little's Law `N = BW x latency / 64 B` is computed in `analyze.py`.
* **`kernel`** - `z=x*y` or `y=a*x+y` on `double` arrays, vectorised by GCC. `order=rand blk=B` visits blocks of B bytes in random order, which
  separates spatial locality (cache lines) from page locality (TLB). The vectorised kernels are validated against a scalar reference at start-up; the process
  aborts if they differ. `passes=N` runs a fixed amount of work so counters can be normalised per element.
* **`sysprobe`** - cache sizes/topology from the OS and an effective core clock from a chain of dependent `imul` instructions (3-cycle latency on x86; a dependent `add` chain is unusable because newer Intel cores fold it at rename).
  All ns -> cycle conversions in the summary use this measured clock.

All timing uses `QueryPerformanceCounter` (Windows) / `CLOCK_MONOTONIC` (Linux) around windows of 60-300 ms, and every number in the plots is the
mean over repeated windows (and 2 random layouts for the latency sweeps) with the standard deviation as error bar.

## Sanity checks to do before trusting a single plot

These are the things a naive or AI-written benchmark gets wrong. Do them and note the outcome in the report.

* Zero-queue **L1 latency should be about 4-5 cycles**. If `lat_zeroq.csv` shows far less, the compiler removed the loads; inspect the loop with
  `objdump -d --no-show-raw-insn bin/lat.exe | less` (look for a run of dependent `mov (%reg),%reg`). During development of this repo the first version of the chase loop
  was deleted by the optimiser (0.001 ns per load) until the sink variable was made `volatile` - which is exactly why this check matters.
* **Sequential chase over a DRAM-sized buffer is not DRAM latency** (hardware prefetch). The `patterns` experiment shows the size of that error.
* The DRAM-sized buffer must be at least about 4x the L3. `run_all.py` prints a warning otherwise.
* `huge_ok` columns: 1 means large pages were really granted. If you asked for them and it says 0 the TLB-with-large-pages result is meaningless.
* Unpinned or power-saver runs are deliberately included as pitfall demonstrations - do not use them for the main results.

## Honest limitations of this toolchain

* The prebuilt Windows binaries are baseline x86-64 (SSE2 vectors, built with zig/clang); a GCC `-march=native` build can reach higher in-cache kernel bandwidth. DRAM-bound results are not affected.
* Windows has no `perf`; per-miss counter data needs VTune / AMD uProf or a Linux boot (`docs/COUNTERS.md`). Without counters the cache/TLB analysis relies on
  latency transitions only.
* Intel MLC is not used; `loaded` reproduces its idea (separate latency probe + throttled load generators) but its absolute numbers will differ, mostly because it
  does not use MLC's hugepage-backed buffers and its delay unit is core cycles of a spin loop, not MLC's injection delay.
* The random-bandwidth generator samples blocks with replacement from a power-of-two number of blocks.
* Hybrid (P+E core) CPUs: restrict to P-cores with `--cores`, and say so in the report.
* Virtual machines and WSL2 add a second level of address translation and hide counters; do not use them for the TLB experiments.
* Only single-socket, single-NUMA-node machines are handled (no NUMA placement control).

## Repository layout

```
src/        C sources (common.h, kern.h, lat.c, bw.c, loaded.c, kernel.c, sysprobe.c, runner.c)
scripts/    build.py, sysinfo.py, run_all.py, analyze.py, counter_plan.py
docs/       NO_INSTALL.md, WINDOWS_SETUP.md, COUNTERS.md
data/raw/   one CSV per experiment (appended by run_all.py)       data/processed/  aggregated stats
data/sysinfo.json, build_info.json, run_meta.json                  results/figs, results/tables, results/summary.md
HYPOTHESES.md   REPORT.md
```
