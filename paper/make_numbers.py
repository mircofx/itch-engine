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
         "table-fib W=1024": "fibo", "table-fib W=512": "fibh", "global-id": "gid", "global-fib": "gfib",
         "fib-L8 W=8192": "fibl", "fib-L16 W=8192": "fib", "fib-L8 W=16384": "fibw"}


def load_counters(path, expect):
    """-> (stats per variant, {rep: {variant: (rate, miss/msg)}}, reps). fibl/fibw = 8-byte Level at W=8192/16384."""
    raw, byrep = {}, {}
    for line in path.read_text().splitlines():
        m = re.match(r"^(\d+)\s+(\S+),\s+W=(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", line)
        if not m:
            continue
        rep, kind, w, rate, ins, mis, ref, mpm = m.groups()
        key = f"{kind} W={w}" if kind in ("table-fib", "fib-L8", "fib-L16") else kind
        raw.setdefault(names[key], []).append((float(rate), float(ins), float(mpm)))
        byrep.setdefault(int(rep), {})[names[key]] = (float(rate), float(mpm))
    reps = {len(v) for v in raw.values()}
    assert len(raw) == expect and len(reps) == 1, f"{path.name}: expected {expect} variants, equal runs: {[(k, len(v)) for k, v in raw.items()]}"
    stats = {}
    for nm, runs in raw.items():
        rate, ins, mpm = zip(*runs)
        assert max(ins) - min(ins) < 0.015, f"{path.name} {nm}: instruction count should be deterministic, got {ins}"
        stats[nm] = {"R": st.median(rate), "Rlo": min(rate), "Rhi": max(rate), "I": st.median(ins),
                     "P": st.median(mpm), "Plo": min(mpm), "Phi": max(mpm)}
    return stats, byrep, reps.pop()


C, byrep, nreps = load_counters(CNT, 10)
for nm, s in C.items():
    for k, v in s.items():
        macros[f"C{nm}{k}"] = f"{v:.1f}" if k.startswith("R") else f"{v:.2f}"
    lines_out.append(f"counters {nm:5s} rate={s['R']:.1f} [{s['Rlo']:.1f},{s['Rhi']:.1f}] "
                     f"instr={s['I']:.2f} miss/msg={s['P']:.2f} [{s['Plo']:.2f},{s['Phi']:.2f}]")

def pct(x):
    return f"{x * 100:.0f}\\%"

fib, mp, idt, w512, gid, gfib = C["fib"], C["map"], C["id"], C["fibh"], C["gid"], C["gfib"]
macros["CountersReps"] = str(nreps)
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

# paired: in how many interleaved rounds did a variant miss more than per-book Fibonacci W=8192 (16-byte Level, the old default)
# in the same round. For the 8-byte variants, which beat it, also count rounds with fewer misses and with strictly higher throughput.
def paired(byrep, nm, ref="fib"):
    n = len(byrep)
    more = sum(1 for r in byrep.values() if r[nm][1] > r[ref][1])
    fewer = sum(1 for r in byrep.values() if r[nm][1] < r[ref][1])
    faster = sum(1 for r in byrep.values() if r[nm][0] > r[ref][0])
    return f"{more}/{n}", f"{fewer}/{n}", f"{faster}/{n}"

for nm in C:
    if nm == "fib":
        continue
    more, fewer, faster = paired(byrep, nm)
    macros[f"C{nm}Pair"] = more
    lines_out.append(f"paired {nm:5s} vs fib8192: more misses {more}, fewer {fewer}, faster {faster}")
for nm in ("fibl", "fibw"):
    _, fewer, faster = paired(byrep, nm)
    macros[f"C{nm}Fewer"], macros[f"C{nm}Faster"] = fewer, faster

# 8-byte Level: session 1 (VARIANT_SET=level, 9 rounds) and session 2 (inside the full matrix above)
fibl, fibw = C["fibl"], C["fibw"]
macros["PctLevelMiss"] = pct(1 - fibl["P"] / fib["P"])
macros["PctLevelWideMiss"] = pct(1 - fibw["P"] / fib["P"])
macros["PctLevelSpeed"] = pct(fibl["R"] / fib["R"] - 1)
macros["PctLevelWideSpeed"] = pct(fibw["R"] / fib["R"] - 1)
macros["PctLevelInstr"] = pct(fibl["I"] / fib["I"] - 1)
macros["PctLevelWideInstr"] = pct(fibw["I"] / fib["I"] - 1)
LVL = R / "counters_level_reps9.txt"
S1, s1rep, s1n = load_counters(LVL, 3)
macros["SoneReps"] = str(s1n)
for nm, tag in (("fib", "Base"), ("fibl", "L"), ("fibw", "W")):
    macros[f"Sone{tag}P"], macros[f"Sone{tag}R"] = f"{S1[nm]['P']:.2f}", f"{S1[nm]['R']:.1f}"
    macros[f"Stwo{tag}P"], macros[f"Stwo{tag}R"] = f"{C[nm]['P']:.2f}", f"{C[nm]['R']:.1f}"
for nm, tag in (("fibl", "L"), ("fibw", "W")):
    _, fewer, faster = paired(s1rep, nm)
    macros[f"Sone{tag}Fewer"], macros[f"Sone{tag}Faster"] = fewer, faster
macros["PctSoneWideMiss"] = pct(1 - S1["fibw"]["P"] / S1["fib"]["P"])
macros["PctSoneWideSpeed"] = pct(S1["fibw"]["R"] / S1["fib"]["R"] - 1)
lines_out.append(f"level session1: base {S1['fib']['P']:.2f}/{S1['fib']['R']:.1f}  L8 {S1['fibl']['P']:.2f}/{S1['fibl']['R']:.1f}  "
                 f"L8w {S1['fibw']['P']:.2f}/{S1['fibw']['R']:.1f}  pairs {paired(s1rep, 'fibl')} {paired(s1rep, 'fibw')}")
lines_out.append(f"level session2: base {fib['P']:.2f}/{fib['R']:.1f}  L8 {fibl['P']:.2f}/{fibl['R']:.1f}  "
                 f"L8w {fibw['P']:.2f}/{fibw['R']:.1f}  pairs {paired(byrep, 'fibl')} {paired(byrep, 'fibw')}")

# diagnostics (bench_diag.sh, -DPROBE_STATS, untimed): overflow count per width, probe lengths per table
D = {}
for line in (R / "diag.txt").read_text().splitlines():
    m = re.match(r"^diag (\S+) overflow=(\d+) calls=(\d+) slots=(\d+) avg=([\d.]+) max=(\d+) maxrss_kb=(\d+)", line)
    if m:
        nm, ov, calls, slots, avg, mx, rss = m.groups()
        D[nm] = {"ov": int(ov), "avg": float(avg), "max": int(mx), "calls": int(calls), "rss": int(rss)}
        lines_out.append(f"diag {nm:10s} overflow={int(ov):>10,} probe avg={float(avg):.3f} max={mx} rss={int(rss) / 1048576:.2f} GiB")
assert {"id8192", "fib8192", "fib512", "gid8192", "gfib8192", "fib8192l8", "fib16384l8"} <= D.keys(), f"diag.txt incomplete: {sorted(D)}"
for nm, d in D.items():
    tag = {"id8192": "id", "fib8192": "fib", "fib4096": "fibf", "fib1024": "fibo", "fib512": "fibh",
           "gid8192": "gid", "gfib8192": "gfib", "fib8192l8": "fibl", "fib16384l8": "fibw"}[nm]
    macros[f"D{tag}Ov"] = f"{d['ov'] / 1e6:.1f}"
    macros[f"D{tag}Avg"] = f"{d['avg']:.2f}"
    macros[f"D{tag}Max"] = f"{d['max']:,}".replace(",", "{,}")
    macros[f"D{tag}Rss"] = f"{d['rss'] / 1048576:.1f}"
macros["PctOvWide"] = pct(1 - D["fib16384l8"]["ov"] / D["fib8192"]["ov"])   # overflow adds removed by W=16384

# full-day check that a 32-bit share total can't wrap (bench_levelmax.sh)
lm = (R / "levelmax_fullday.txt").read_text()
m = re.search(r"messages : (\d+).*?levelmax : shares=(\d+) \(([\d.]+)% of UINT32_MAX\) orders=(\d+)", lm, re.S)
assert m and "asserts: none fired" in lm, "levelmax_fullday.txt incomplete"
macros["LmMsgs"] = f"{int(m.group(1)):,}".replace(",", "{,}")
macros["LmShares"] = f"{int(m.group(2)):,}".replace(",", "{,}")
macros["LmSharesM"] = f"{int(m.group(2)) / 1e6:.1f}"
macros["LmPct"] = f"{float(m.group(3)):.2f}\\%"
macros["LmHeadroom"] = f"{4294967295 / int(m.group(2)):.0f}"
macros["LmOrders"] = f"{int(m.group(4)):,}".replace(",", "{,}")
lines_out.append(f"levelmax full day: shares={m.group(2)} ({m.group(3)}% of UINT32_MAX, headroom {macros['LmHeadroom']}x) orders={m.group(4)}")
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
