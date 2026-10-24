# Hardware counters for the cache-miss and TLB-miss experiments

The rubric awards 35 points for working-set / cache / TLB analysis and says counters must be "used correctly". Windows has no `perf`, so pick one route.

## Step 1 - prepare the runs

```
python scripts/counter_plan.py
```
This runs nine kernel configurations un-profiled (about 4 s each) and writes `data/raw/counters.csv` with runtime and element counts filled in and the counter
columns empty. It prints one command line per row, for example `bin\kernel.exe size=268435456 kernel=mul order=rand blk=64 huge=0 cpu=2 passes=15 reps=1`.
The runs do a fixed number of passes, so the counter **totals** divided by `elements` give misses per element.

## Step 2 - collect totals for each command (choose one route)

**A. Intel CPU: Intel VTune Profiler** (free standalone download). Hardware event-based sampling needs the sampling driver (installed with VTune, run elevated).
Use a custom hardware-event analysis and read the event counts for the whole process. The exact CLI knobs and event names depend on VTune version and
microarchitecture, so check `vtune -help collect` / the event reference for your CPU. You want: core cycles, instructions retired, retired loads that missed L1, L2, L3
(`MEM_LOAD_RETIRED.L1_MISS / L2_MISS / L3_MISS` on recent cores), and DTLB load misses that caused a page walk (`DTLB_LOAD_MISSES.WALK_COMPLETED`).

**B. AMD CPU: AMD uProf** (free). Use a custom event profile and select: cycles, retired instructions, data-cache (L1) misses/refills, L2 misses, L3 misses, L1 DTLB
and L2 DTLB misses. Again, event names depend on the Zen generation; take them from the uProf event list for your CPU.

**C. Any CPU: boot Linux** (live USB is enough if the PMU is exposed; not inside a VM/WSL2) and run
```
sudo perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses ./bin/kernel size=... passes=... reps=1
```
Rebuild with `python3 scripts/build.py` first. Use the same command lines printed by `counter_plan.py`.

I have not been able to test routes A and B on real hardware: treat the event names above as pointers, verify them with the tool's own documentation, and record the
exact events/tool version in the report. If your machine exposes no usable counters, say so in the report; the latency-transition evidence (`fig_ws_latency`, `fig_tlb`,
`fig_kernel_cache`) is then the only support for the cache/TLB claims and you should word them as inferences.

## Step 3 - fill `data/raw/counters.csv`

Columns to fill (leave unavailable ones empty): `cycles, instructions, l1_miss, l2_miss, llc_miss, dtlb_miss, stlb_miss`. Totals for the whole process, not per-second rates.
Sampling profilers report scaled estimates; if your tool offers counting mode ("event counts", `perf stat`) prefer that.

## Step 4 - analyse

```
python scripts/analyze.py
```
produces `results/figs/fig_counters.png` (runtime per element vs misses per element for each counter) and Pearson/Spearman correlations in `results/summary.md`.

## How to interpret (what the report should say)

* Sequential configurations: hardware prefetchers fetch lines ahead of the loads, so *demand* miss counts can be far below the number of lines actually moved.
  Compare the counts with the measured bandwidth (24 B per element) rather than expecting one miss per line.
* `rand blk=64` vs `rand blk=4096`: same total bytes, but 64 B blocks pay a cache miss and a page walk per block, 4 KiB blocks pay one page walk per 512 elements.
  Large pages should remove most DTLB misses for the 4 KiB case.
* A counter that does not correlate with runtime is a result too (for example a miss that is fully overlapped by memory-level parallelism). Say why.
* AMAT check: `AMAT = t_L1 + m_L1 * (t_L2 + m_L2 * (t_L3 + m_L3 * t_DRAM))` with the zero-queue latencies from `results/tables/zero_queue.md` and your measured miss
  *ratios* (misses / accesses; accesses = 2 loads + 1 store per element for z=x*y, counted the same way your counter counts them). Compare with the measured ns/element and explain the gap (overlap, prefetch, TLB).
