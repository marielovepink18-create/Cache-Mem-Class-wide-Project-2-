# Windows setup (native Windows 10/11, no WSL)

## 1. Compiler (GCC from MSYS2)

1. Install MSYS2 from <https://www.msys2.org> (default folder `C:\msys64`).
2. Open the **MSYS2 UCRT64** shell and run:
   ```
   pacman -Syu
   pacman -S mingw-w64-ucrt-x86_64-gcc
   ```
   (the first command may ask you to close the shell and run it again).
3. Add `C:\msys64\ucrt64\bin` to your Windows `PATH` (Settings -> System -> About -> Advanced system settings -> Environment Variables), then open a **new** PowerShell and check:
   ```
   gcc --version
   ```
4. Python 3.9+ from python.org or the Microsoft Store, then:
   ```
   pip install -r requirements.txt
   ```
   `python scripts/build.py` uses whichever `gcc` is first on PATH (or the `CC` environment variable).

MSVC is not supported (the code uses GCC builtins and inline assembly).

## 2. Make the machine quiet and fixed-frequency

* **Plug in AC power** (laptops) and choose the high-performance profile. In an admin PowerShell:
  ```
  powercfg /list                    # note the GUIDs
  powercfg /setactive SCHEME_MIN    # "High performance" alias; on some laptops use Settings -> Power mode -> Best performance
  powercfg /getactivescheme         # the report needs this line
  ```
  `sysinfo.py` records the active plan automatically.
* Close browsers, Teams/Discord, cloud-sync clients, game launchers. Leave the machine alone while the run is going (about half an hour).
* Do not change antivirus settings. If scans are running in the background, wait for them to finish.
* Note whether **SMT/Hyper-Threading** is on (`sysinfo.py` prints it). You may leave it on; the scripts pin one thread per physical core. Just state it.
* **Hybrid Intel CPUs (12th gen and newer)**: P-cores are normally the lowest-numbered cores. Use `python scripts/run_all.py --cores <number of P-cores>` so no
  benchmark thread lands on an E-core, and mention it in the report. Cache sizes differ between core types; `sysinfo.py` warns if it sees that.
* Laptops throttle: keep the lid open, watch temperature if you can (HWiNFO is fine) and note it. Repeat a suspicious curve after a cool-down.

## 3. Large pages (needed for the TLB experiment's second half)

Windows only gives large pages to processes holding the "Lock pages in memory" privilege.

1. Win+R -> `secpol.msc` -> Local Policies -> User Rights Assignment -> **Lock pages in memory** -> Add your user.
2. **Sign out and back in** (or reboot).
3. Run `python scripts/run_all.py --only tlb --quick`. In `data/raw/lat_tlb.csv` the `huge_ok` column must be `1` for the `huge_req=1` rows.

Windows Home does not ship `secpol.msc`. If you cannot grant the privilege, the benchmark falls back to 4 KiB pages and records `huge_ok=0`; state that plainly in the
report rather than presenting those rows as large-page results. Large allocations can also fail from fragmentation - reboot and retry before anything else.

## 4. First run

```
python scripts/build.py
python scripts/sysinfo.py          # keep this output for the report's methodology section
python scripts/run_all.py --quick
python scripts/analyze.py
```
Open `results\figs\*.png` and `results\summary.md`. If something looks implausible, work through "Sanity checks" in the README before the full run.

## 5. Frequency-policy pitfall (manual, about 2 minutes)

```
powercfg /setactive SCHEME_MIN
python scripts/run_all.py --only pitfall_freq --tag highperf
powercfg /setactive SCHEME_MAX     # Power saver
python scripts/run_all.py --only pitfall_freq --tag powersaver
powercfg /setactive SCHEME_MIN     # restore
python scripts/analyze.py
```

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `gcc` not found | PATH not updated or old terminal; open a new PowerShell |
| `allocation failure` | not enough free RAM for the thread count; lower `--max-mem-gb` |
| `huge_ok` is 0 | privilege missing (section 3) or memory fragmented |
| warning `could not pin to cpu N` | more than 64 logical CPUs (processor groups unsupported) or invalid CPU number |
| L1 latency looks like 0.3 ns | loop optimised away; see README sanity checks |
| numbers jump between runs | background load, thermal throttling, or power plan; repeat after fixing and keep the bad run as an anomaly |
