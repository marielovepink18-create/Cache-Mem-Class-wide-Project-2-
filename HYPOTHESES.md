# Pre-experiment hypotheses


## Machine (from `python scripts/sysinfo.py`)
CPU AMD Athlon Gold 315 0U | cores/threads 2/4 | L1d 32 KiB | L2 512 KiB per core | L3 4 MiB | memory 4GB + 8GB, 2400 MT/s, 2 channels

Theoretical DRAM bandwidth = 2400 MT/s x 8 B x 2 channels = 38.4 GB/s 

## 1. Zero-queue latency (random dependent chase, 4 KiB pages)
| level | predicted cycles | predicted ns | reasoning |
|---|---|---|---|
| L1 | 4  | 1.7ns | This is the small and fastest memory level.|
| L2 | 13  | 5.4ns | Secondary memory cache that has more storage space than L1 but operates at a slower speed. |
| L3 | 50  | 20.8ns | Acts as a shared storage pool that the entire processor can access and is way slower than the other two despite having the most memory.|
| DRAM | 288 | 120ns | The data isn't within the cache levels (cache miss) so now it has to send the request to DRAM. |

The latency-vs-size curve should show plateaus with steps near ___ KiB, ___ and ___ MiB. The steps will be gradual rather than sharp because ___ (associativity, replacement, TLB).

## 2. Pattern / granularity / stride
* Sequential vs random chase over a DRAM-sized buffer: I expect a 20x difference, with sequential access being faster, because 
* there are prefetchers that hide its latency. The prefetchers also fetches lines before they're asked and so when they are loaded it is 
* already in the cache. So there are fewer cache misses. A random chase can’t be predicted, so the CPU can’t prefetch it.
* A few loads may hit in the cache by chance, but because the buffer is much larger than the cache, almost all of them miss and wait the full DRAM latency.
* Bandwidth vs access granularity (64 B ... 4 KiB): I expect to get more bytes per trip and a higher bandwidth because when you're 
* doing a small random access the CPU doesn't have enough information to predict where you want to go next. Also since 
* it's a small random access you aren't doing it enough to form a pattern. However, as you access more memory the CPU can form
* a prediction on where you'll go and the prefetchers will come into work.
* Strides of 64, 256, 1024 B (one line touched per stride): useful bandwidth should drop because for strided access some lines are skipped over.
* Even though the memory will still grab the whole line, useful bandwidth only counts the bytes that were actually read.
* Therefore, the bigger the stride, the lower the useful bandwidth. A 64 B stride is the same as sequential access: it goes in order without 
* skipping any lines, so its useful bandwidth is the biggest. Bigger strides also make the pattern harder for the prefetcher to follow.

## 3. Read/write mix
* Normal stores: write-heavy mixes deliver half of the read-only bandwidth because it must first read back into the cache 
* and then later write it back.
* Non-temporal stores: these write directly into memory without the line being read first by the CPU because of this 
* they are about 1.3x to 2x faster than a normal store.

## 4. Throughput-latency (intensity) curve
* Idle latency 120 ns. Latency stays within 30% of idle until about 60% of peak bandwidth, then rises sharply (the knee).
* Peak bandwidth 25 GB/s = 65% of theoretical. Concurrency from Little's Law at peak: N = BW x latency / 64 B = 25 x 120 / 64 = 47 lines in flight.
* One core alone can sustain only about 10 outstanding misses (N_core), giving a single-core limit of N_core x 64 B / latency = 5.3 GB/s.

## 5. Cache and TLB
* A random 64 B-block visit order over DRAM-sized arrays will be 3x slower than sequential because random order loses
* prefetching and pays the full amount on of a miss on every block.
* * Using 4 KiB-sized random blocks instead recovers 65% of that because, even though the jumps between blocks are random, the reads inside each block
* are in order, so the prefetchers come into play and one miss and page lookup is shared by the whole block.
* TLB: with one node per page, latency starts rising when the footprint exceeds 64 pages (L1 DTLB 64 entries, L2 TLB 1536 entries => 256 KiB / 6 MiB of reach).
  Large pages should reduce the TLB penalty because one 2 MiB page covers 512 times more memory than a 4 KiB page, so 
* far fewer translations are needed.

## 6. Pitfalls I expect to see
* Not pinning: more run-to-run variation, because the OS can move the thread between cores, which loses its warm caches and can
* put it on a busy core.
* Power-saver / non-fixed frequency: cache latencies in ns will go up while DRAM latency will change much less because it depends 
* mostly on the memory chips.
* Sequential chase mistaken for latency: it would report a latency far too low, a few ns instead of about 120 ns, 
* because the prefetcher hides the memory latency.
