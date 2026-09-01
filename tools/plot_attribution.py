#!/usr/bin/env python3
"""Plot the P&L decomposition produced by tools/sweep.cpp.

    ./build/mms_sweep > docs/attribution.csv
    python3 tools/plot_attribution.py docs/attribution.csv docs/attribution.png
"""
import csv, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src = sys.argv[1] if len(sys.argv) > 1 else "docs/attribution.csv"
out = sys.argv[2] if len(sys.argv) > 2 else "docs/attribution.png"

rows = list(csv.DictReader(open(src)))
live   = [r for r in rows if r["frozen"] == "0"]
frozen = [r for r in rows if r["frozen"] == "1"]

H = 5.0   # headline horizon for the left panel

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.5, 4.9))
fig.patch.set_facecolor("white")

# ---- left: decomposition against informed-flow intensity -------------------
sel = sorted([r for r in live if float(r["horizon"]) == H],
             key=lambda r: float(r["informed"]))
x  = [float(r["informed"]) for r in sel]
sp = [float(r["spread_capture"])    for r in sel]
ad = [float(r["adverse_selection"]) for r in sel]
tot= [float(r["total"])             for r in sel]

floor = [float(r["adverse_selection"]) for r in frozen if float(r["horizon"]) == H]
if floor:
    ax1.axhspan(min(floor), 0, color="0.90", zorder=0)
    ax1.text(x[-1], min(floor)/2, " price-impact floor\n (no information)",
             va="center", ha="right", fontsize=8, color="0.35")

ax1.axhline(0, color="0.6", lw=0.8, zorder=1)
ax1.plot(x, sp,  "o-", color="#1a7f37", lw=2, ms=5, label="spread capture")
ax1.plot(x, ad,  "o-", color="#cf222e", lw=2, ms=5, label="adverse selection")
ax1.plot(x, tot, "o--", color="#0969da", lw=2, ms=5, label="net P&L")
ax1.set_xlabel("informed arrival rate  (events per unit time)")
ax1.set_ylabel("ticks per lot")
ax1.set_title(f"Spread capture is flat. Adverse selection carries the loss.\n"
              f"(horizon = {H:g} time units)", fontsize=10.5, loc="left")
ax1.legend(frameon=False, fontsize=9)
ax1.grid(alpha=0.25, lw=0.6)

# ---- right: adverse-selection curve by horizon ------------------------------
by_inf = defaultdict(list)
for r in live:
    by_inf[float(r["informed"])].append((float(r["horizon"]),
                                         float(r["adverse_selection"])))

shades = ["#fdbf6f", "#fb9a4a", "#ef6548", "#cf222e", "#8b0a1a"]
for c, inf in zip(shades, [1.0, 2.0, 3.0, 4.0, 8.0]):
    pts = sorted(by_inf[inf])
    ax2.plot([p[0] for p in pts], [p[1] for p in pts],
             "o-", color=c, lw=1.9, ms=4, label=f"informed = {inf:g}")

pts = sorted(by_inf[0.0])
ax2.plot([p[0] for p in pts], [p[1] for p in pts],
         "o-", color="0.45", lw=1.9, ms=4, label="informed = 0")

ax2.set_xscale("log")
ax2.axhline(0, color="0.6", lw=0.8)
ax2.set_xlabel("markout horizon  (time units, log scale)")
ax2.set_ylabel("adverse selection, ticks per lot")
ax2.set_title("Adverse selection deepens with horizon, then flattens\n"
              "as the information is fully priced in", fontsize=10.5, loc="left")
ax2.legend(frameon=False, fontsize=8.5, loc="lower left")
ax2.grid(alpha=0.25, lw=0.6)

fig.suptitle("Market maker P&L attribution — 120,000 events, 8 seeds",
             fontsize=12, fontweight="bold", x=0.008, ha="left", y=0.995)
fig.tight_layout(rect=(0, 0, 1, 0.95))
fig.savefig(out, dpi=160)
print(f"wrote {out}")
