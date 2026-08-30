"""Chart a 3-minute iperf3 run over Thunderbolt networking: throughput, CPU
on both ends, the Windows effective clock, and die temperature on both
machines.  Writes SVG and PNG.

  linkchart.py <rundir> <outbase>

The Windows temperatures come from Dell Command | Monitor's WMI provider
(root/dcim/sysman, DCIM_NumericSensor).  Dell reports UnitModifier = -1 but
CurrentReading is already whole degrees C on this hardware.
"""
import json
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

rundir, outbase = sys.argv[1], sys.argv[2]


def load_iperf(path):
    """iperf3 --logfile appends, so the file can hold several runs; take the last."""
    raw = open(path).read()
    dec = json.JSONDecoder()
    docs, i = [], 0
    while i < len(raw):
        while i < len(raw) and raw[i] in " \r\n\t":
            i += 1
        if i >= len(raw):
            break
        obj, i = dec.raw_decode(raw, i)
        docs.append(obj)
    return docs[-1]


d = load_iperf(rundir + r"\iperf3.json")
ip_t = [s["sum"]["start"] for s in d["intervals"]]
ip_r = [s["sum"]["bits_per_second"] / 8e6 for s in d["intervals"]]
mean_mbps = d["end"]["sum_sent"]["bits_per_second"] / 8e6


def read_csv(path):
    rows = []
    for line in open(path):
        if not line.strip() or line[0] == "t":
            continue
        rows.append([float(x) for x in line.strip().split(",")])
    return rows


win = read_csv(rundir + r"\win-samples.csv")   # t,cpu,perf,mhz,tx,rx,cpu_c,amb_c
lin = read_csv(rundir + r"\lin-samples.csv")   # t,cpu,temp,rx


def align(rows, busy_col, thresh=200.0):
    """Shift a sampler's clock so the transfer starts at t=0."""
    for r in rows:
        if r[busy_col] > thresh:
            return [[x - r[0] if i == 0 else x for i, x in enumerate(q)] for q in rows]
    return rows


win = align(win, 4)
lin = align(lin, 3)

w_t = [r[0] for r in win]
w_cpu = [r[1] for r in win]
w_clk = [r[2] for r in win]
w_tx = [r[4] for r in win]
w_tmp = [r[6] for r in win]
l_t = [r[0] for r in lin]
l_cpu = [r[1] for r in lin]
l_tmp = [r[2] for r in lin]

INK = "#1b1b1f"
GRID = "#d8d8de"
C_PAY = "#1f6fb4"
C_WIRE = "#8fb8d8"
C_WIN = "#c2410c"
C_LIN = "#0f766e"
C_CLK = "#7c3aed"

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9, "text.color": INK,
    "axes.labelcolor": INK, "axes.edgecolor": "#9a9aa2", "axes.linewidth": 0.8,
    "xtick.color": INK, "ytick.color": INK, "figure.facecolor": "white",
    "axes.facecolor": "white", "svg.fonttype": "none",
})

fig, ax = plt.subplots(4, 1, figsize=(10, 9.5), sharex=True,
                       gridspec_kw={"height_ratios": [3, 2, 2, 2], "hspace": 0.18})

fig.suptitle("Thunderbolt networking, 3 minutes of iperf3 — Windows → Linux, one stream",
             x=0.055, ha="left", y=0.975, fontsize=13, fontweight="bold")
fig.text(0.055, 0.943,
         "i5-8350U ↔ Ryzen 7 PRO 7840U, MTU 65330, link negotiated at 20 Gbit/s",
         ha="left", fontsize=9, color="#55555c")

# 1: throughput
a = ax[0]
a.plot(w_t, w_tx, color=C_WIRE, lw=1.0, label="on the wire (NIC counter, both ends agree)")
a.plot(ip_t, ip_r, color=C_PAY, lw=1.4, label="payload (iperf3)")
a.axhline(mean_mbps, color=C_PAY, lw=0.8, ls=":", alpha=0.8)
a.set_ylabel("MB/s")
a.set_ylim(0, 2100)
a.yaxis.set_major_locator(MultipleLocator(500))
a.annotate("mean %.0f MB/s = %.1f Gbit/s payload" % (mean_mbps, mean_mbps * 8 / 1000),
           xy=(3, mean_mbps), xytext=(3, mean_mbps + 190), color=C_PAY,
           fontsize=9, fontweight="bold")
a.annotate("flat for the whole run: first 30 s %.0f, last 30 s %.0f MB/s"
           % (sum(ip_r[:30]) / 30, sum(ip_r[-30:]) / 30),
           xy=(0.985, 0.08), xycoords="axes fraction", ha="right",
           fontsize=8.5, color="#55555c")
a.legend(loc="lower right", frameon=False, fontsize=8.5, ncol=2,
         bbox_to_anchor=(1.0, 0.18))

# 2: CPU, both ends, one shared range
a = ax[1]
a.plot(w_t, w_cpu, color=C_WIN, lw=1.3, label="Windows (4 cores / 8 threads)")
a.plot(l_t, l_cpu, color=C_LIN, lw=1.3, label="Linux (16 threads)")
a.set_ylabel("CPU, % of\nall cores")
a.set_ylim(0, 100)
a.yaxis.set_major_locator(MultipleLocator(25))
a.legend(loc="upper right", frameon=False, fontsize=8.5, ncol=2)
a.annotate("the link costs almost no CPU — nothing here is the limit",
           xy=(0.015, 0.72), xycoords="axes fraction", fontsize=8.5, color="#55555c")

# 3: Windows effective clock
a = ax[2]
a.plot(w_t, w_clk, color=C_CLK, lw=1.3)
a.axhline(100, color="#9a9aa2", lw=0.8, ls="--")
a.set_ylabel("Windows clock,\n% of base")
a.set_ylim(0, 220)
a.yaxis.set_major_locator(MultipleLocator(50))
a.annotate("100% = 1.7 GHz base", xy=(178, 104), ha="right", fontsize=8, color="#77777e")
busy_clk = [r[2] for r in win if r[4] > 200]
a.annotate("turbo held all run (%.0f–%.0f%% while transferring), even at 82 °C: "
           "it is sustained power draw that throttles, not heat alone"
           % (min(busy_clk), max(busy_clk)),
           xy=(0.015, 0.16), xycoords="axes fraction", fontsize=8.5, color="#55555c")

# 4: die temperature, both ends, one shared range
a = ax[3]
a.plot(w_t, w_tmp, color=C_WIN, lw=1.3, label="Windows CPU probe (Dell Command | Monitor)")
a.plot(l_t, l_tmp, color=C_LIN, lw=1.3, label="Linux die (acpitz)")
a.set_ylabel("die temp, °C")
a.set_ylim(30, 95)
a.yaxis.set_major_locator(MultipleLocator(15))
a.set_xlabel("seconds")
a.set_xlim(-4, 184)
a.xaxis.set_major_locator(MultipleLocator(20))
a.legend(loc="lower right", frameon=False, fontsize=8.5, ncol=2)
busy_w = [r[6] for r in win if r[4] > 200]
a.annotate("Windows %.0f → %.0f °C, Linux %.0f → %.0f °C — both flat by 60 s"
           % (busy_w[0], max(busy_w), l_tmp[0], max(l_tmp)),
           xy=(0.015, 0.90), xycoords="axes fraction", fontsize=8.5, color="#55555c")

for a in ax:
    a.grid(True, axis="y", color=GRID, lw=0.7)
    a.set_axisbelow(True)
    for s in ("top", "right"):
        a.spines[s].set_visible(False)

fig.text(0.055, 0.058,
         "iperf3 3.21, no rsync or ssh involved — this is the link on its own, measured at a8664bae.  "
         "Windows: Dell Latitude 7490, Intel Thunderbolt driver 1.41.1423.\n"
         "Windows temperatures from Dell Command | Monitor (root/dcim/sysman, DCIM_NumericSensor); its "
         "UnitModifier of −1 is wrong, the reading is already °C.\n"
         "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  © 2026 Max Vilimpoc",
         ha="left", va="top", fontsize=8, color="#666666")

fig.subplots_adjust(left=0.115, right=0.975, top=0.905, bottom=0.155)
fig.savefig(outbase + ".svg", format="svg")
fig.savefig(outbase + ".png", format="png", dpi=160)
print("wrote %s.svg and %s.png" % (outbase, outbase))
