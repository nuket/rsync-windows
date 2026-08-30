"""Charts for the ISA-L AES-GCM backend in the bundled ssh.exe.

Two three-minute runs per backend, alternating cng/isal/isal/cng, each from
a 60 C start: one ssh connection fed from memory, so the only thing being
measured is the cipher and what the machine does to it as it heats.

  isalcharts.py <outdir>
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import MultipleLocator

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
OUT = sys.argv[1]

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "svg.fonttype": "none",
})

CNG = "#e08b4a"        # Windows' own
ISAL = "#1f77b4"       # Intel's assembly
GREEN = "#2ca02c"
ACCENT = "#d62728"
INK = "#1b1b1f"

FOOT = ("Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W), Balanced power plan  ·  "
        "Linux: Ryzen 7 PRO 7840U  ·  Thunderbolt, MTU 65330, 20 Gbit/s\n"
        "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  "
        "one ssh connection, aes128-gcm, fed from memory  ·  "
        "ISA-L 2.26.1, six files, AVX2 path only  ·  © 2026 Max Vilimpoc")


def load(name):
    rows = []
    with open(os.path.join(DATA, name), newline="") as f:
        for r in csv.DictReader(f):
            if not r["MBps"]:
                continue
            rows.append((float(r["t"]), float(r["MBps"]),
                         float(r["clockPct"]), float(r["tempC"])))
    return rows


RUNS = {
    "cng": [load("thermal-cng-1.csv"), load("thermal-cng-2.csv")],
    "isal": [load("thermal-isal-1.csv"), load("thermal-isal-2.csv")],
}


def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=200,
                    bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", name)


def fifth(rows, col, last):
    n = max(1, len(rows) // 5)
    part = rows[-n:] if last else rows[:n]
    return sum(r[col] for r in part) / len(part)


# --------------------------------------------------------------------------
# 1. The three minutes, as they happened
# --------------------------------------------------------------------------
def timeline():
    fig, axes = plt.subplots(3, 1, figsize=(11, 9.6), sharex=True,
                             gridspec_kw={"height_ratios": [1.5, 1, 1]})
    ax1, ax2, ax3 = axes

    for backend, colour, label in (("cng", CNG, "Windows CNG"),
                                   ("isal", ISAL, "ISA-L (AVX2 assembly)")):
        for i, rows in enumerate(RUNS[backend]):
            t = [r[0] for r in rows]
            style = "-" if i == 0 else "--"
            ax1.plot(t, [r[1] for r in rows], style, color=colour, lw=1.6,
                     label=(label if i == 0 else None))
            ax2.plot(t, [r[2] for r in rows], style, color=colour, lw=1.4)
            ax3.plot(t, [r[3] for r in rows], style, color=colour, lw=1.4)

    ax1.set_ylabel("MB/s through one ssh connection")
    ax1.set_ylim(1000, 1700)
    ax1.yaxis.set_major_locator(MultipleLocator(100))
    ax1.legend(frameon=False, loc="lower left", fontsize=10.5)
    ax1.set_title("Three minutes at line rate from a 60 °C start: both throttle, one keeps its throughput",
                  fontsize=14, fontweight="bold", loc="left", pad=12)
    for backend, colour in (("cng", CNG), ("isal", ISAL)):
        a = sum(fifth(r, 1, False) for r in RUNS[backend]) / 2
        b = sum(fifth(r, 1, True) for r in RUNS[backend]) / 2
        ax1.annotate("%+.0f%%" % (100 * (b - a) / a), xy=(178, b),
                     xytext=(6, -4), textcoords="offset points", fontsize=11,
                     fontweight="bold", color=(ACCENT if b < a * 0.97 else GREEN),
                     va="center")

    ax2.set_ylabel("effective clock,\n% of base")
    ax2.set_ylim(100, 200)
    ax2.yaxis.set_major_locator(MultipleLocator(25))
    ax2.annotate("both end at the same clock — the throttling is identical",
                 xy=(92, 138), fontsize=10, color="#555555", ha="center")

    ax3.set_ylabel("CPU probe, °C")
    ax3.set_ylim(55, 95)
    ax3.yaxis.set_major_locator(MultipleLocator(10))
    ax3.set_xlabel("seconds into the run   (solid = first run, dashed = second)")
    ax3.set_xlim(0, 190)
    ax3.xaxis.set_major_locator(MultipleLocator(30))

    for ax in axes:
        ax.grid(True, color="#e2e2e8", lw=0.8)
        ax.set_axisbelow(True)

    fig.subplots_adjust(hspace=0.12)
    fig.text(0.01, 0.045,
             "Each run starts once the CPU probe has come back to 60 °C, and the four runs alternate "
             "cng/isal/isal/cng so neither backend gets\nthe cold machine twice. The load is one ssh "
             "connection fed from memory — no disk, no rsync — so the cipher is what is being measured.\n"
             + FOOT, fontsize=8, color="#666666", ha="left", va="top",
             transform=fig.transFigure)
    save(fig, "windows-perf-tb-isal-thermal")


# --------------------------------------------------------------------------
# 2. What that costs, start against sustained
# --------------------------------------------------------------------------
def summary():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 5.2),
                                   gridspec_kw={"width_ratios": [1.2, 1]})

    w = 0.36
    x = [0, 1]
    first = [sum(fifth(r, 1, False) for r in RUNS[b]) / 2 for b in ("cng", "isal")]
    last = [sum(fifth(r, 1, True) for r in RUNS[b]) / 2 for b in ("cng", "isal")]
    # each backend keeps its own colour; the cold bar is the pale one, so
    # the legend can be about cold-against-hot without lying about which
    # colour belongs to which backend
    b1 = ax1.bar([i - w / 2 for i in x], first, w, color=["#f2cdae", "#a9c9e2"])
    b2 = ax1.bar([i + w / 2 for i in x], last, w, color=[CNG, ISAL])
    for bars in (b1, b2):
        for b in bars:
            ax1.annotate("%.0f" % b.get_height(),
                         xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                         xytext=(0, 6), textcoords="offset points", ha="center",
                         fontsize=10, fontweight="bold")
    for i in x:
        ax1.annotate("%+.1f%%" % (100 * (last[i] - first[i]) / first[i]),
                     xy=(i + w / 2, last[i]), xytext=(0, 26),
                     textcoords="offset points", ha="center", fontsize=12,
                     fontweight="bold",
                     color=ACCENT if last[i] < first[i] * 0.97 else GREEN)
    ax1.set_xticks(x)
    ax1.set_xticklabels(["Windows CNG", "ISA-L"], fontsize=11)
    ax1.set_ylabel("MB/s, mean of two runs")
    ax1.set_ylim(0, 1750)
    ax1.yaxis.set_major_locator(MultipleLocator(250))
    ax1.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax1.set_axisbelow(True)
    ax1.legend(handles=[Patch(facecolor="#dcdcdc", label="first 36 s (cold)"),
                        Patch(facecolor="#8a8a8a", label="last 36 s (hot)")],
               frameon=False, fontsize=10, loc="upper left")
    ax1.set_title("Cold, they are close; hot, they are not", fontsize=12,
                  fontweight="bold", loc="left")

    # what the machine was doing meanwhile
    labels = ["clock,\n% of base", "CPU probe,\n°C"]
    cng_v = [sum(fifth(r, 2, True) for r in RUNS["cng"]) / 2,
             sum(fifth(r, 3, True) for r in RUNS["cng"]) / 2]
    isal_v = [sum(fifth(r, 2, True) for r in RUNS["isal"]) / 2,
              sum(fifth(r, 3, True) for r in RUNS["isal"]) / 2]
    xx = [0, 1]
    c1 = ax2.bar([i - w / 2 for i in xx], cng_v, w, color=CNG, label="CNG")
    c2 = ax2.bar([i + w / 2 for i in xx], isal_v, w, color=ISAL, label="ISA-L")
    for bars in (c1, c2):
        for b in bars:
            ax2.annotate("%.0f" % b.get_height(),
                         xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                         xytext=(0, 6), textcoords="offset points", ha="center",
                         fontsize=10, fontweight="bold")
    ax2.set_xticks(xx)
    ax2.set_xticklabels(labels, fontsize=10.5)
    ax2.set_ylabel("in the last 36 s")
    ax2.set_ylim(0, 170)
    ax2.yaxis.set_major_locator(MultipleLocator(25))
    ax2.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax2.set_axisbelow(True)
    ax2.legend(frameon=False, fontsize=10, loc="upper left")
    ax2.set_title("at the same clock and the same heat", fontsize=12,
                  fontweight="bold", loc="left")

    fig.subplots_adjust(wspace=0.3)
    fig.text(0.01, -0.02,
             "The point is not the peak — cold, the two are within 4% of each other. It is that the same "
             "throttling costs CNG 6–11% and ISA-L 1%.\nOn a 15 W laptop a long transfer runs hot for almost "
             "all of its length, so the right-hand bars are the ones a real transfer lives in.\n" + FOOT,
             fontsize=8, color="#666666", ha="left", va="top")
    save(fig, "windows-perf-tb-isal")


timeline()
summary()
