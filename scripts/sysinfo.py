#!/usr/bin/env python3
"""Collect the system configuration the rubric asks for -> data/sysinfo.json (+ prints a Markdown table)."""
import json, os, platform, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""


def sh(cmd, shell=False):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, shell=shell, timeout=60).stdout.strip()
    except Exception:
        return ""


def powershell(q):
    return sh(["powershell", "-NoProfile", "-Command", q])


def sysprobe(cpu):
    exe = ROOT / "bin" / ("sysprobe" + EXE)
    if not exe.exists():
        sys.exit("bin/sysprobe missing - run: python scripts/build.py")
    out = sh([str(exe), f"cpu={cpu}"])
    d = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip()
    return d


def linux_phys_cpus():
    seen, res = set(), []
    base = Path("/sys/devices/system/cpu")
    for c in sorted(base.glob("cpu[0-9]*"), key=lambda p: int(p.name[3:])):
        f = c / "topology/thread_siblings_list"
        if not f.exists():
            continue
        s = f.read_text().strip()
        if s not in seen:
            seen.add(s)
            res.append(int(re.split(r"[,-]", s)[0]))
    return res


def main():
    info = {"os": platform.platform(), "python": sys.version.split()[0], "machine": platform.machine()}
    bi = ROOT / "data" / "build_info.json"
    if bi.exists():
        info.update(json.loads(bi.read_text()))
    # first probe (unpinned) for topology, then pin to a physical core for the frequency estimate
    p = sysprobe(-1)
    phys = [int(x) for x in p.get("physical_cpus", "").split(",") if x != ""]
    if os.name != "nt" and not phys:
        phys = linux_phys_cpus()
    if not phys:
        phys = list(range(int(p.get("logical_cpus", 1))))
    cal_cpu = phys[1] if len(phys) > 1 else phys[0]
    p2 = sysprobe(cal_cpu)
    info["cache"] = {k: int(float(p[k])) for k in ("l1d_bytes", "l2_bytes", "l3_bytes", "line_bytes")}
    info["cache_heterogeneous"] = p.get("cache_heterogeneous") == "1"
    info["logical_cpus"] = int(p.get("logical_cpus", 0))
    info["physical_cpus"] = phys
    info["physical_cores"] = len(phys)
    info["smt_enabled"] = info["logical_cpus"] > len(phys)
    info["large_page_min"] = int(p.get("large_page_min", 0))
    info["measured_core_ghz"] = float(p2.get("calib_ghz", 0))
    info["freq_calibration_cpu"] = cal_cpu

    if os.name == "nt":
        info["cpu_model"] = powershell("(Get-CimInstance Win32_Processor | Select -First 1).Name")
        info["cpu_max_mhz_reported"] = powershell("(Get-CimInstance Win32_Processor | Select -First 1).MaxClockSpeed")
        info["power_plan"] = sh("powercfg /getactivescheme", shell=True)
        dimm = powershell("Get-CimInstance Win32_PhysicalMemory | ForEach-Object { '{0} GB @ {1} MT/s (configured {2}), {3}' -f ($_.Capacity/1GB), $_.Speed, $_.ConfiguredClockSpeed, $_.PartNumber }")
        info["dimms"] = [x.strip() for x in dimm.splitlines() if x.strip()]
        info["total_ram_gb"] = powershell("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)")
        info["on_battery"] = powershell("(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue).BatteryStatus")
    else:
        info["cpu_model"] = next((l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines() if l.startswith("model name")), "")
        gov = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        info["governor"] = gov.read_text().strip() if gov.exists() else "n/a"
        thp = Path("/sys/kernel/mm/transparent_hugepage/enabled")
        info["thp"] = thp.read_text().strip() if thp.exists() else "n/a"
        info["total_ram_gb"] = round(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2**30, 1)
        info["kernel"] = platform.release()

    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "sysinfo.json").write_text(json.dumps(info, indent=2))
    c = info["cache"]
    print("| item | value |\n|---|---|")
    print(f"| CPU | {info['cpu_model']} |")
    print(f"| physical cores / logical CPUs | {info['physical_cores']} / {info['logical_cpus']} (SMT {'on' if info['smt_enabled'] else 'off'}) |")
    print(f"| L1d / L2 / L3 | {c['l1d_bytes']//1024} KiB / {c['l2_bytes']//1024} KiB / {c['l3_bytes']//1048576} MiB (line {c['line_bytes']} B) |")
    print(f"| measured core clock (dependent-imul chain, cpu {cal_cpu}) | {info['measured_core_ghz']:.2f} GHz |")
    print(f"| physical CPUs used for pinning | {phys} |")
    print(f"| OS | {info['os']} |")
    print(f"| compiler | {info.get('compiler','?')} |")
    if os.name == "nt":
        print(f"| power plan | {info['power_plan']} |")
        print(f"| DIMMs | {'; '.join(info['dimms'])} |")
    else:
        print(f"| governor / THP | {info['governor']} / {info['thp']} |")
    if info["cache_heterogeneous"]:
        print("\nNOTE: cache sizes differ between cores (hybrid CPU). Pin to P-cores (usually the lowest CPU numbers) and say so in the report.")


if __name__ == "__main__":
    main()
