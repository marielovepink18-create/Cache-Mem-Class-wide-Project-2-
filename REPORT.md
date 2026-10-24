# Project 2: Cache & Memory Performance Profiling

Name: Marie Morales  |  Course: ECSE 4320  |  Date: 2026-10-23 


## 1. AI-use note

**Tool used:** Claude (Anthropic) chat assistant.
**What it generated:** the C benchmark sources in `src/`, the Python drivers in `scripts/`, the README/setup skeleton, the report/hypothesis skeleton, and assisted in writing the report.

**What I independently did**
- Built the benchmark tools on my own laptop (AMD Athlon Gold 3150U, Windows) and fixed the setup problems that came up, including pointing the build at CLion's bundled gcc and fixing a locked runner.exe that blocked the build.
- Ran the quick test and then the full set of experiments, and re-ran the loaded-latency experiment after we found the load generator was too slow to show a knee.
- Wrote my own pre-experiment hypotheses from datasheet-style expectations in HYPOTHESES.md.
- Read the source files. lat.c measures latency with a pointer chase, where each load depends on the previous one so nothing can be overlapped. bw.c 
- measures bandwidth by streaming through buffers with different block sizes, strides, orders and read/write mixes. loaded.c measures how 
- latency rises while other threads load the memory. kernel.c runs z = x*y and saxpy over arrays in different visit orders to show cache and TLB effects.
- Checked the cache latencies and TLB sizes against the AMD/WikiChip data sheet for my CPU.
- Checked the draft analysis against the figures and the data, and rewrote the explanations in my own words.

## 2. Pre-experiment hypotheses
I expected the Zen+ cache latencies of about 4, 13 and 50 cycles and DRAM around 120 ns, a large gap between sequential and random access 
caused by prefetching, write-heavy mixes at about half of read bandwidth, a sharp latency knee near 60% of peak bandwidth, 
and random small blocks to be much slower than large blocks.

Comparison with the results:
- **Cache latencies:** right in cycles (about 4, 12 and 47) but too low in ns, because the laptop ran at about 1.6 GHz and not the 2.4 GHz I assumed.
- **DRAM latency:** wrong. I predicted 120 ns and measured 197 ns, probably because of TLB misses with 4 KiB pages, slow laptop memory and the low clock.
- **Sequential vs random:** right in direction. I predicted 20x and measured 30x.
- **Read/write mix:** right. Write-heavy ran at 43% of read-only, and non-temporal stores were 1.9x faster.
- **Bandwidth:** peak was 21.1 GB/s (55%), lower than the 65% I predicted, probably because of the mismatched DIMMs and only 2 cores.
- **Knee:** wrong. I expected a sharp knee, but the latency rose only about 25% at 90% of peak, so there was no sharp knee.
- **One core's outstanding misses:** about 8 measured against my guess of 10, but its bandwidth was only about 2.1 GB/s, well below my predicted 5.3 GB/s.
- **Cache and TLB:** random 64 B blocks were 5.4x slower, more than my 3x, and 4 KiB blocks recovered almost all of it.
- **Pitfalls:** pinning mattered less than I expected, and a power plan change moved L2 latency by about 7% and DRAM by about 4%.


## 3. Methodology 
- CPU: AMD Athlon Gold 3150U, 2 cores / 4 threads (SMT on), L1d 32 KiB, L2 512 KiB, L3 4 MiB, 64 B lines.
- Memory: 4 GB + 8 GB DDR4, running at 2400 MT/s (rated 3200). Theoretical peak = 2400 x 8 B x 2 channels = 38.4 GB/s at most; mismatched DIMM sizes may mean part of the memory is single-channel.
- OS: Windows 11 (build 22631), laptop, power plan "HP Recommended" for the main run, Best performance mode selected. Compiler: GCC 15.2.0 (CLion's bundled MinGW), flags `-O3 -march=native -std=gnu11 -fno-tree-loop-distribute-patterns`.
- Measured core clock (dependent-IMUL chain): about 1.61-1.65 GHz, also with Best performance selected, so the laptop was probably power- or thermally limited. Cycle numbers below use that clock.
- Pinning: single-thread tests on logical CPU 2; latency thread on CPU 0; load threads used the other logical CPUs including hyper-thread siblings (only 2 physical cores).
- DRAM-sized buffer: 256 MiB (64x the L3). Each point: 5 timed windows of 100-300 ms, 2 random layouts for latency sweeps, error bars = 1 standard deviation.
- Large pages were **not** granted (`huge_ok = 0` in every large-page row): no "Lock pages in memory" right.
- No hardware counters were collected.
- Memory cap during benchmarks: 1 GB total.
- Quick pilot run was done before the full run; this was mentioned in HYPOTHESES.md. **[YOU: confirm]**

### Validation and sanity checks
| check | expected | observed |
|---|---|---|
| L1 chase latency in cycles | 4-5 | about 4 (2.43 ns at about 1.6 GHz) |
| Kernel correctness (vectorised vs scalar reference, automatic in `kernel`) | pass | passed (run finished with 0 failures) |
| DRAM-sized buffer >= 4x L3 | yes | yes (256 MiB = 64x the 4 MiB L3) |
| Peak DRAM bandwidth vs theoretical | 60-90% | 55% (21.1 of 38.4 GB/s) - slightly below the expected range |
| Large pages actually granted | yes | **no** (`huge_ok = 0`) |
| Run-to-run variation of the headline numbers | < 5% | DRAM latency s.d. 4.4 ns (about 2%); L3-sized latency about 25% (noisy) |

Tools and what each measures: see `README.md` ("How the tools measure").Changes made while getting the tools to work on my laptop (together with the assistant): 
the clock estimate was switched from a dependent-add chain, which reported an impossible 15-19 GHz, to a dependent-IMUL 
chain; the pointer-chase result was made volatile because the compiler had removed the loop (0.001 ns per load); 
a C runner (runner.c) replaced the Python scripts because I could not install Python on the measured PC; 
and the load generators in loaded.c were changed to work on 1 KiB blocks because the first version could only produce 11 GB/s and showed no knee.

## 4. Zero-queue latency
| level | buffer | latency | cycles @1.6 GHz |
|---|---|---|---|
| L1 | 16 KiB | 2.43 ns | about 4 |
| L2 | 256 KiB | 7.27 ns | about 12 |
| L3 | 2 MiB | 29 ns (noisy, s.d. 6.7 ns) | about 47 |
| DRAM | 256 MiB | 197 ns (s.d. 4.4) | about 320 |

The latency-vs-size curve shows four plateaus: about 2.4 ns (L1), about 7 ns (L2), about 17-30 ns (L3) and about 200 ns (DRAM), 
with steps just above 32 KiB, 512 KiB and 4 MiB, matching the L1d, L2 and L3 sizes. In cycles at the measured 1.6 GHz clock these are about 
4, 12, 47 and 320 cycles. The steps are gradual rather than sharp for several reasons: caches are set-associative, so some addresses 
compete for the same sets before the cache is completely full; replacement is not perfect LRU; and a working set only slightly 
larger than a cache still gets some hits. TLB misses add to the slope at larger sizes, which is why latency keeps rising from 
about 100 ns to 200 ns between 4 MiB and 256 MiB even though the data no longer fits in any cache. The capacity-only model uses sharp cutoffs, 
so it underestimates latency between about 1 and 4 MiB, where the real L3 behaves worse than "capacity only" (set conflicts, sharing, TLB misses). 
Above 4 MiB it overestimates, because the model assumes every access above L3 costs the full DRAM latency.
Compared with my hypothesis: my cycle predictions (4, 13 and 50) were close to the measured 4, 12 and about 47, but my ns predictions 
were too low because I assumed a 2.4 GHz clock and the laptop ran at about 1.6 GHz, and my DRAM guess of 120 ns was too low compared with the measured 197 ns.

## 5. Pattern, granularity, stride
- Sequential vs random pointer chase over 256 MiB: **6.9 ns vs 205 ns per load (30x)**. The sequential number is not a memory latency: hardware prefetchers fetch the next lines before the loads need them. It grows to 8.9 ns at 256 B stride and 14.1 ns at 1 KiB stride as prefetching works less well.
- Bandwidth vs access granularity (reads, 1 thread / 2 threads, GB/s):
  | granularity | seq 1 thr | seq 2 thr | rand 1 thr | rand 2 thr |
  |---|---|---|---|---|
  | 64 B | 7.0 | 12.0 | 1.0 | 1.6 |
  | 256 B | 10.7 | 16.3 | 2.2 | 3.2 |
  | 1 KiB | 10.7 | 19.5 | 3.0 | 5.7 |
  | 4 KiB | 12.2 | 21.1 | 6.8 | 11.0 |
  Random 64 B accesses reach only 1.0 GB/s per thread, about 14% of sequential; larger blocks amortise the miss and the page-walk cost, and random 4 KiB blocks reach about 52-56% of sequential.
- Strided reads, one 64 B line per stride (`bw_stride.md`): useful bandwidth falls from 7.6 GB/s (stride 64 B, contiguous) to 2.8 GB/s at 128 B and about 2.1-2.5 GB/s at larger strides (1 thread); with 2 threads from 11.0 to 4.4 and then 3.2-3.9 GB/s. Skipping lines defeats the stream prefetcher, so the core falls back to demand misses; the plateau of about 2.1-2.5 GB/s per thread is the same as the single-core limit found in the MLP test (section 7).
- **Peak read bandwidth:** 21.1 GB/s (2 threads, sequential, 4 KiB blocks), which is 55% of the theoretical 38.4 GB/s. 
- One thread alone reaches 12.2 GB/s. A likely reason for the gap is that this CPU has only 2 physical cores, and each core can keep only about 8 memory requests 
- in flight at once (see the MLP test in section 7), so two cores cannot create enough outstanding requests to use all of the memory bandwidth. 
- This is a hypothesis I did not test.
Comparison with my hypothesis: I predicted a 20x gap between sequential and random and measured 30x, and I was right that 
- bandwidth rises with block size and that useful bandwidth falls with stride. My predicted peak was 25 GB/s (65% of theoretical) 
- and the measured peak was 21.1 GB/s (55%), so the limit of two cores' outstanding misses, which I had estimated at about 10 and 
- measured at about 8, is a likely reason for the shortfall.

## 6. Read/write mix

Figure: `fig_rw_mix.png`. Bandwidth of bytes touched, 1 KiB blocks, sequential (GB/s):

| mix | 1 thread, normal stores | 1 thread, non-temporal | 2 threads, normal | 2 threads, non-temporal |
|---|---|---|---|---|
| 100% read | 13.6 | - | 20.6 | - |
| 70/30 | 10.3 | 10.5 | 12.4 | 14.0 |
| 50/50 | 8.1 | 10.6 | 10.5 | 14.0 |
| 100% write | 6.8 | 12.7 | 8.9 | 16.9 |

- **Normal stores:** write-heavy mixes are much slower. With 2 threads, 100% write reaches only 43% of 100% read (8.9 vs 20.6 GB/s). A normal store first reads the line into the cache and later writes it back, so each byte written costs about twice the memory traffic.
- **Non-temporal stores** skip that first read. 100% write goes from 8.9 to 16.9 GB/s with 2 threads (1.9x) and from 6.8 to 12.7 GB/s with 1 thread (1.9x).
- **Under load:** the latency seen by a separate chase thread rises from 243 ns (100% read) to 300 ns (70/30), 334 ns (50/50) and 377 ns (100% write), while the load threads' bandwidth falls from 18.7 to 9.8 GB/s. Writes put more traffic on the memory bus for fewer useful bytes, so the chase thread waits longer.
- **Compared with my prediction:** I said write-heavy would be about half of read-only (measured 43%) and non-temporal stores 1.3x to 2x faster (measured 1.9x). Both were in the right range.

## 7. Latency under load and Little's Law

Figures: `fig_intensity.png`, `fig_chains.png`.

- **Idle latency** of the chase thread is 197 ns. Peak bandwidth from 3 load threads is 18.1 GB/s.
- **No sharp knee.** Latency rises only a little as load grows. The first point at 90% of peak is 17.3 GB/s at 246 ns, only 1.25x idle. It never reached 2x idle. One likely reason, which I did not test, is that 2 cores and 4 threads cannot make enough requests to fill the memory system, so queues stay short. The load threads also shared cores through hyper-threading.
- **Little's Law:** N = bandwidth x latency / 64 B gives the number of cache lines in flight. It grows in a straight line with bandwidth and reaches about 70 lines at the heaviest load (18.1 GB/s x 246 ns / 64 B). That is more than two cores can sustain as normal misses. A possible reason, untested, is that the streaming loads also have prefetch requests in flight.
- **One core** (`fig_chains.png`): with K independent chains on one core, latency stays at about 200 ns up to 4 chains, rises to 245 ns at 8 chains and 350 ns at 12. Bandwidth grows from 0.32 GB/s (1 chain) to 2.09 GB/s at 8 chains, then stays near 2.0-2.4 GB/s. So one core can keep about 8 misses in flight. More chains only wait in line inside the core. That is a one-core limit of 8 x 64 B / 245 ns = 2.1 GB/s.
- **Compared with my prediction:** I said there would be a sharp knee near 60% of peak (wrong, the curve is soft). I said peak 25 GB/s (measured 18.1 in this test and 21.1 in the plain bandwidth test), about 47 lines in flight (measured about 70), about 10 outstanding misses per core (measured about 8) and a one-core limit of 5.3 GB/s (measured about 2.1). The one-core limit was too high because idle latency was 197 ns, not the 120 ns I assumed.

## 8. Working set, cache misses and TLB misses

Figures: `fig_ws_bandwidth.png`, `fig_kernel_cache.png`, `fig_tlb.png`.

- **Bandwidth vs working set:** the kernel traffic (24 B per element) is about 40-46 GB/s while the arrays fit in L1 or L2. It falls to about 14-17 GB/s once they are bigger than the 4 MiB L3 (saxpy a little higher than mul). The simple read test is limited by its own loop in cache (about 18-22 GB/s), so only the kernel shows the true in-cache speed. One thread reading a DRAM-sized buffer gets about 11 GB/s.
- **Visit order** (`fig_kernel_cache.png`; 128 MiB arrays, `z = x*y`, ns per element): sequential 2.10; random order of 64 KiB blocks 2.02; 4 KiB blocks 2.45; 1 KiB 4.66; 256 B 7.17; 64 B 11.30. Random 64 B blocks are **5.4x slower** than sequential for the same arithmetic and the same bytes. 4 KiB blocks are only 17% slower, so they recover about 96% of the lost time. Most of the penalty comes from losing cache-line and prefetch benefits, not from page effects inside a block.
- **TLB** (`fig_tlb.png`): the TLB is a small cache of address translations. With one node per 4 KiB page, latency stays at the L1 level (about 2.5 ns) up to an address range of about 1 MiB (about 250 pages). It jumps to about 7.7 ns at 1.4 MiB (about 360 pages), rises slowly to about 13 ns at 16 MiB (about 4,000 pages), then climbs faster: 22 ns at 23 MiB, 34 ns at 32 MiB, about 160 ns at 64 MiB, and 200 ns at 256 MiB. The normal curve rises at the cache sizes, so this curve shows where the translation caches run out instead.
- **Data-sheet check:** for Zen+, the L1 data TLB has 64 entries (fully associative) and the L2 data TLB has 1,536 entries (12-way). That is a reach of 64 x 4 KiB = 256 KiB and 1,536 x 4 KiB = 6 MiB (sources: 7-cpu.com Zen page; WikiChip Zen+ page, which lists 1,532 entries for the L2 TLB). I did not check that the 3150U matches these figures exactly. My measurements do not match them cleanly: the first rise is at about 250-360 pages, not 64, and the bigger rise starts at about 4,000-6,000 pages (16-23 MiB), well past 1,536 entries. I do not have a tested explanation. My prediction of a rise at about 64 pages was wrong for this machine.
- **Large pages were not granted**, so the "large pages" results are the same as the 4 KiB results (201 vs 203 ns at 256 MiB, and the kernel results are unchanged). I cannot draw any conclusion about large pages from this run.
- **Compared with my prediction:** I said random 64 B blocks would be 3x slower than sequential (measured 5.4x) and that 4 KiB blocks would recover 65% (measured about 96%).
- Hardware counters were not available, so claims about cache misses and TLB misses come from latency and bandwidth changes only.

## 9. Benchmark pitfalls

Figure: `fig_pitfalls.png`.

1. **Sequential chase mistaken for DRAM latency:** 6.9 ns vs 205 ns (30x), see section 5. I predicted it would show only a few ns, which was right.
2. **Threads not pinned** (15 windows each): L2-sized latency varied 0.6% when pinned and 3.4% when not. DRAM-sized latency showed no difference (1.2% vs 0.9%). Read bandwidth was 12.3 +/- 1.1 GB/s pinned and 11.7 +/- 1.3 GB/s not pinned. The effect is real but small on this lightly loaded 2-core laptop. The L3-sized buffer is noisy either way (about 25%), possibly because the small shared L3 is easily disturbed. I did not test that.
3. **Power plan / clock:** switching to power-saver changed L2 latency from 7.23 to 7.72 ns (+7%) and DRAM from 226 to 236 ns (+4%). L1 stayed at 2.4 ns. This matches my prediction that cache latency in ns would go up and DRAM would change less, but the effect was small, probably because the clock was already low (about 1.6 GHz) in both modes. The clock must be measured, not assumed: the same 7.2 ns for L2 is about 12 cycles at 1.65 GHz but would be about 22 cycles at a 3 GHz boost clock.
4. **Compiler removing the loop:** in an early version the chase reported 0.001 ns per load until the result was made `volatile`. A correct L1 result is about 4 cycles.
5. **Load generator too slow:** the first loaded-latency version could only make 11 GB/s, so no knee could appear. Working on 1 KiB blocks raised the load to 18 GB/s, and I re-ran the experiment after the fix.

## 10. Anomalies (surprises)

- **Clock about 1.6 GHz even on Best performance.** Probably limited by power or heat. Next test: log the clock during a run, on AC power with a cooling pad.
- **DRAM latency 197 ns, higher than the 120 ns I expected.** The chase uses 4 KiB pages, so it includes TLB misses and page walks, and the memory is slow (2400 MT/s) on a laptop with a low clock. Next test: repeat with large pages.
- **L3 latency varies by about 25%.** Probably interference on the small shared L3 and the TLB. Next test: more repetitions on an idle system.
- **Peak bandwidth only 55% of the best case** (21.1 of 38.4 GB/s). Possible causes: mismatched DIMMs, and only 2 cores with about 8 outstanding misses each. Next test: check the memory setup and compare with another bandwidth tool.
- **No sharp latency jump** (only 1.25x idle at 90% of peak). Probably because 2 cores and 4 threads cannot fill the memory system. Next test: a machine with more cores.
- **About 70 lines in flight by Little's Law, but only about 8 per core.** Prefetch requests may count. Next test: a load generator with random access or no prefetch.
- **First TLB rise at about 250-360 pages, not 64.** Next test: a finer sweep of page counts, and check the data sheet for the 3150U.
- **Large pages not available**, so the TLB effect cannot be separated from the cache effect.
- **Pinning had only a small effect.**

## 11. Limitations

- Windows laptop with no `perf`, so no hardware counters. Conclusions about cache and TLB misses rest on latency curves only. No Intel MLC cross-check.
- No large pages ("Lock pages in memory" right not granted).
- Only 2 real cores with hyper-threading, so the load threads shared cores with each other and with the chase thread.
- The clock (about 1.6 GHz) was not controlled. Cycle numbers use the measured clock.
- Mismatched DIMMs (4 GB + 8 GB), so memory may not run fully dual-channel.
- Small samples: 5 timed windows per point (2 random layouts for latency sweeps), and the L3-sized numbers are noisy.
- Single socket, no NUMA control, normal desktop OS noise. The loaded-latency delay is counted in spin-loop cycles, not Intel MLC's unit, and random bandwidth picks blocks with replacement.

## 12. How to reproduce

```
python scripts/build.py && python scripts/sysinfo.py && python scripts/run_all.py && python scripts/analyze.py
```
On a PC without Python or installs: run `build.bat`, then `bin\runner.exe` (see `docs/NO_INSTALL.md`). Raw data is in `data/raw/`. Tables are in `data/processed/` and `results/tables/`. The full run took about **[YOU: minutes]** minutes on my laptop.

**Sources:** [7-cpu.com, AMD Zen](https://7-cpu.com/cpu/Zen.html); [WikiChip, Zen+](https://en.wikichip.org/wiki/amd/microarchitectures/zen%2B)

On a PC without Python/installs: `build.bat` then `bin\runner.exe` (see `docs/NO_INSTALL.md`). Raw data: `data/raw/`. Aggregated: `data/processed/`, `results/tables/`. 
