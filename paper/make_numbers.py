#!/usr/bin/env python3
"""Turn raw benchmark logs in results/ into LaTeX macros (numbers_auto.tex).

usage: make_numbers.py <results_dir> <out.tex>
Rules applied here (also stated in the paper):
  - throughput = messages / internal steady_clock seconds (not process wall time)
  - m3_full.txt pair 1 is dropped: an apt install overlapped it
"""
import re
import statistics as st
import sys
from pathlib import Path

R = Path(sys.argv[1])
OUT = Path(sys.argv[2])
MSGS = {"itch_2gb.bin": 64858229, "01302019.NASDAQ_ITCH50": 368366634}


def load_m3(path, drop_pairs=()):
    header = path.read_text().splitlines()[0]
    fname = re.search(r"file=(\S+)", header).group(1).split("/")[-1]
    n = MSGS[fname]
    rates = {"single": [], "threaded": []}
    for line in path.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        mode, run, secs, *_ = line.split()
        if int(run) in drop_pairs:
            continue
        rates[mode].append(n / float(secs) / 1e6)
    return rates


def stats(xs):
    return st.median(xs), st.stdev(xs), st.mean(xs), min(xs), max(xs)


macros = {}
lines_out = []
for key, fn, drop in (("Slice", "m3_slice.txt", ()), ("Full", "m3_full.txt", (1,))):
    r = load_m3(R / fn, drop)
    s, t = stats(r["single"]), stats(r["threaded"])
    macros[f"{key}N"] = f"{len(r['single'])}+{len(r['threaded'])}"
    macros[f"{key}Smed"], macros[f"{key}Ssd"] = f"{s[0]:.2f}", f"{s[1]:.2f}"
    macros[f"{key}Tmed"], macros[f"{key}Tsd"] = f"{t[0]:.2f}", f"{t[1]:.2f}"
    d = (t[0] / s[0] - 1) * 100
    macros[f"{key}Delta"] = (f"$-${abs(d):.1f}\\%" if d < 0 else f"+{d:.1f}\\%")
    # paired comparison: pair i = single run i vs threaded run i (interleaved)
    wins = sum(1 for a, b in zip(r["single"], r["threaded"]) if b > a)
    macros[f"{key}Wins"] = f"{wins}/{min(len(r['single']), len(r['threaded']))}"
    lines_out.append(f"{key}: single med={s[0]:.3f} sd={s[1]:.3f} mean={s[2]:.3f} [{s[3]:.2f},{s[4]:.2f}] | "
                     f"threaded med={t[0]:.3f} sd={t[1]:.3f} mean={t[2]:.3f} [{t[3]:.2f},{t[4]:.2f}] | "
                     f"delta={macros[key + 'Delta']} threaded-wins={macros[key + 'Wins']}")

# counters: REPS interleaved runs per variant, raw rows "<rep> <label> <rate> <instr G> <miss G> <refs G> <miss/msg>".
# Medians and ranges are computed here from the raw rows, not taken from the script's own summary.
CNT = R / "counters_reps5.txt"
names = {"map": "map", "table-id": "id", "table-fib W=8192": "fib", "table-fib W=4096": "fibf",
         "table-fib W=1024": "fibo", "table-fib W=512": "fibh", "global-id": "gid", "global-fib": "gfib"}
raw = {}
byrep = {}   # rep -> {variant: miss/msg}, for paired comparisons within the same interleaved round
for line in CNT.read_text().splitlines():
    m = re.match(r"^(\d+)\s+(\S+),\s+W=(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", line)
    if not m:
        continue
    rep, kind, w, rate, ins, mis, ref, mpm = m.groups()
    key = f"{kind} W={w}" if kind == "table-fib" else kind
    raw.setdefault(names[key], []).append((float(rate), float(ins), float(mpm)))
    byrep.setdefault(int(rep), {})[names[key]] = float(mpm)
reps = {len(v) for v in raw.values()}
assert len(raw) == 8 and len(reps) == 1, f"expected 8 variants with equal run counts, got {[(k, len(v)) for k, v in raw.items()]}"
C = {}
for nm, runs in raw.items():
    rate, ins, mpm = zip(*runs)
    assert max(ins) - min(ins) < 0.015, f"{nm}: instruction count should be deterministic, got {ins}"
    C[nm] = {"R": st.median(rate), "Rlo": min(rate), "Rhi": max(rate), "I": st.median(ins),
             "P": st.median(mpm), "Plo": min(mpm), "Phi": max(mpm)}
    for k, v in C[nm].items():
        macros[f"C{nm}{k}"] = f"{v:.1f}" if k.startswith("R") else f"{v:.2f}"
    lines_out.append(f"counters {nm:5s} rate={C[nm]['R']:.1f} [{C[nm]['Rlo']:.1f},{C[nm]['Rhi']:.1f}] "
                     f"instr={C[nm]['I']:.2f} miss/msg={C[nm]['P']:.2f} [{C[nm]['Plo']:.2f},{C[nm]['Phi']:.2f}]")

def pct(x):
    return f"{x * 100:.0f}\\%"

fib, mp, idt, w512, gid, gfib = C["fib"], C["map"], C["id"], C["fibh"], C["gid"], C["gfib"]
macros["CountersReps"] = str(reps.pop())
macros["CountersCommit"] = re.search(r"commit=(\S+)", CNT.read_text()).group(1)
macros["PctTableInstr"] = pct(1 - fib["I"] / mp["I"])          # open-addressed (fib) vs unordered_map
macros["PctTableMiss"] = pct(1 - fib["P"] / mp["P"])
macros["TableSpeedup"] = f"{fib['R'] / mp['R']:.1f}"
macros["PctWidthMiss"] = pct(w512["P"] / fib["P"] - 1)          # W=512 vs W=8192
macros["PctWidthInstr"] = pct(w512["I"] / fib["I"] - 1)
macros["PctHashInstr"] = pct(1 - fib["I"] / idt["I"])           # fibonacci vs identity, per-book tables
macros["PctHashMiss"] = pct(1 - fib["P"] / idt["P"])
macros["PctGlobalInstr"] = pct(1 - gfib["I"] / fib["I"])        # global fib table vs per-book fib table
macros["PctGlobalMiss"] = pct(gfib["P"] / fib["P"] - 1)
macros["GidInstrX"] = f"{gid['I'] / fib['I']:.0f}"

# paired: in how many interleaved rounds did a variant miss more than per-book Fibonacci W=8192 in the same round
for nm in C:
    if nm == "fib":
        continue
    worse = sum(1 for r in byrep.values() if r[nm] > r["fib"])
    macros[f"C{nm}Pair"] = f"{worse}/{len(byrep)}"
    lines_out.append(f"paired {nm:5s} more misses than fib8192 in {worse}/{len(byrep)} rounds")

# diagnostics (bench_diag.sh, -DPROBE_STATS, untimed): overflow count per width, probe lengths per table
D = {}
for line in (R / "diag.txt").read_text().splitlines():
    m = re.match(r"^diag (\S+) overflow=(\d+) calls=(\d+) slots=(\d+) avg=([\d.]+) max=(\d+)", line)
    if m:
        nm, ov, calls, slots, avg, mx = m.groups()
        D[nm] = {"ov": int(ov), "avg": float(avg), "max": int(mx), "calls": int(calls)}
        lines_out.append(f"diag {nm:8s} overflow={int(ov):>10,} probe avg={float(avg):.3f} max={mx}")
assert {"id8192", "fib8192", "fib512", "gid8192", "gfib8192"} <= D.keys(), f"diag.txt incomplete: {sorted(D)}"
for nm, d in D.items():
    tag = {"id8192": "id", "fib8192": "fib", "fib4096": "fibf", "fib1024": "fibo", "fib512": "fibh",
           "gid8192": "gid", "gfib8192": "gfib"}[nm]
    macros[f"D{tag}Ov"] = f"{d['ov'] / 1e6:.1f}"
    macros[f"D{tag}Avg"] = f"{d['avg']:.2f}"
    macros[f"D{tag}Max"] = f"{d['max']:,}".replace(",", "{,}")
macros["OvWidthX"] = f"{D['fib512']['ov'] / D['fib8192']['ov']:.1f}"

# where W=8192's overflow adds land: sub-penny, or caught by a window 2x/4x/16x as wide around the same centre, or beyond
ow = None
for line in (R / "diag.txt").read_text().splitlines():
    m = re.match(r"^ovwhere fib8192 subpenny=(\d+) below=([\d,]+) above=([\d,]+)", line)
    if m:
        below = [int(x) for x in m.group(2).split(",")]
        above = [int(x) for x in m.group(3).split(",")]
        ow = {"sub": int(m.group(1)), "b": [b + a for b, a in zip(below, above)], "above": sum(above)}
    m = re.match(r"^diag fib8192 .* maxrss_kb=(\d+)", line)
    if m:
        macros["RssGB"] = f"{int(m.group(1)) / 1024 / 1024:.1f}"
assert ow is not None and "RssGB" in macros, "diag.txt lacks the ovwhere/maxrss lines for fib8192"
tot = D["fib8192"]["ov"]
assert ow["sub"] + sum(ow["b"]) == tot, f"ovwhere buckets {ow} don't add up to overflow={tot}"
for name, v in (("Sub", ow["sub"]), ("Two", ow["b"][0]), ("Four", ow["b"][1]), ("Sixteen", ow["b"][2]), ("Beyond", ow["b"][3])):
    macros[f"Ov{name}"] = f"{v / 1e6:.2f}"
    macros[f"Ov{name}Pct"] = pct(v / tot)
macros["OvAbovePct"] = pct(ow["above"] / tot)
lines_out.append(f"ovwhere fib8192 sub={ow['sub']:,} 2x={ow['b'][0]:,} 4x={ow['b'][1]:,} 16x={ow['b'][2]:,} beyond={ow['b'][3]:,} "
                 f"above={ow['above'] / tot:.0%} rss={macros['RssGB']} GB")
macros["PctFewerLookups"] = pct(1 - D['gfib8192']['calls'] / D['fib8192']['calls'])   # mode 2 probes once per op
macros["LookupsPerBook"] = f"{D['fib8192']['calls'] / 1e6:.1f}"
macros["LookupsGlobal"] = f"{D['gfib8192']['calls'] / 1e6:.1f}"

commit = re.search(r"commit=(\w+)", (R / "m3_slice.txt").read_text()).group(1)
macros["Commit"] = commit

with OUT.open("w") as f:
    f.write("% generated by make_numbers.py from results/ -- do not edit\n")
    for k, v in macros.items():
        f.write(f"\\newcommand{{\\{k}}}{{{v}}}\n")
print("\n".join(lines_out))
