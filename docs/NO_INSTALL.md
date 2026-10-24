# Running without installing anything

You do not need a compiler, Python or admin rights on the machine you are measuring. `bin\` already contains Windows executables
(about 70 KB each, built for any 64-bit Windows 10/11 PC, no AVX required) including `runner.exe`, which does what `run_all.py` + `sysinfo.py` do.
Plotting (`scripts/analyze.py`) is done afterwards on any computer or cloud notebook that has Python with pandas and matplotlib.

## Path A - measure your own PC (preferred, this is the hardware the report should describe)

1. Extract the zip to a normal folder, e.g. `C:\Users\you\cachemem` (do not run things from inside the zip viewer). Avoid OneDrive-synced folders.
2. Plug in AC power, close other programs. Set Settings -> System -> Power -> **Power mode: Best performance** if offered.
3. Double-click `run_quick.bat` (or in PowerShell: `.\bin\runner.exe --quick`). It prints your CPU, caches, measured clock and the plan, then runs ~240 short
   measurements (about 5-10 minutes). Check that it says `0 failures`.
4. Fill in `HYPOTHESES.md` and commit/save it before the next step (the assignment wants predictions before results).
5. Double-click `run_full.bat` (about 30-60 minutes on a modest PC). Do not use the PC meanwhile.
6. Frequency pitfall (2 minutes, no admin needed): with Power mode = Best performance run `.\bin\runner.exe --only pitfall_freq --tag highperf`;
   switch Power mode to **Best power efficiency** and run `.\bin\runner.exe --only pitfall_freq --tag powersaver`; switch back.
7. Zip the folder `data` (it contains `raw\*.csv`, `sysinfo.json`, `run_meta.json`) and upload it where you can run step 8 - or send it to me here.

Memory: the runner uses at most 1024 MB by default (`--mem 1024`). On a 4 GB-RAM PC leave it. If your 4 GB limit is only free *disk* space, nothing here
needs more than a few MB of disk, and you can allow more memory with e.g. `.\bin\runner.exe --mem 3000` for better multi-thread bandwidth numbers.
If the runner warns that the DRAM-sized buffer is smaller than 4x L3, results for "DRAM" are partly cache hits - say so in the report.

Options: `--quick`, `--only lat_ws,loaded` (see `--list`), `--cpu N`, `--cores N` (use only the first N physical cores, e.g. the P-cores of a hybrid CPU), `--mem MB`, `--tag NAME`.

**If Windows blocks the exe:** SmartScreen -> "More info" -> "Run anyway"; right-click the zip -> Properties -> Unblock before extracting also helps.
If a school/work policy blocks unsigned programs entirely, use Path B and say in the report which machine the numbers come from.

Not available without installing something: hardware counters (see `docs/COUNTERS.md`) and large pages (needs the "Lock pages in memory" right). Without them the
`tlb` experiment still runs but the large-page rows carry `huge_ok=0`; report that honestly and treat the cache/TLB conclusions as inferences from latency curves.

## Step 8 - make the plots anywhere with Python (cloud notebook is fine)

Needs `pandas`, `matplotlib`, `numpy` (all preinstalled in Google Colab). Upload this repo folder with your `data/` folder inside, then:
```
python scripts/analyze.py
```
Outputs: `results/figs/*.png`, `results/tables/*`, `results/summary.md`. Download `results/` and `data/` and put them in your repo.

## Path B - fallback: measure in a cloud Linux notebook (only if no exe can be run)

Colab has gcc. In a notebook cell:
```
!python scripts/build.py
!python scripts/run_all.py --quick --max-mem-gb 3
!python scripts/analyze.py
```
Caveats you must state: the numbers describe a shared virtual machine, not your PC; pinning and frequency control are limited, counters and huge pages are normally
unavailable, and noise is higher. `scripts/sysinfo.py` records what the VM reports.
