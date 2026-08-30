"""Charts for the xperf round of the rsync-windows speed work
(WINDOWS-PORT.md, "Moar Speed Pt. X! Windows Performance Analyzer"), plus one
overall chart covering the whole optimisation arc from 521ad8ad.

Series come from the CSVs beside this file, which are the run logs the session
wrote (throughput, per-process CPU, effective clock and CPU temperature for
every run).

  python optimization/charts/xperfcharts.py <outdir>
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator, FixedLocator, FuncFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data")
OUT = sys.argv[1] if len(sys.argv) > 1 else "."

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "svg.fonttype": "none",
})

BEFORE = "#b8b8b8"
AFTER = "#1f77b4"
SEND = "#1f6fb4"
RECV = "#8fb8d8"
ACCENT = "#d62728"
GREEN = "#2ca02c"
AMBER = "#e08b4a"
PURPLE = "#7c3aed"
INK = "#1b1b1f"
HASH = "#7a4fd0"

FOOT = ("Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W)  ·  "
        "Linux: Ryzen 7 PRO 7840U (16 threads)  ·  Thunderbolt networking, MTU 65330, link 20 Gbit/s\n"
        "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  "
        "4 GB transfers, aes128-gcm, source file warm in the page cache  ·  "
        "© 2026 Max Vilimpoc")


def load(name):
    with open(os.path.join(DATA, name), newline="", encoding="utf-8-sig") as fh:
        return list(csv.DictReader(fh))


def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=200,
                    bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", name)


def footer(fig, extra=None, y=-0.02):
    fig.text(0.01, y, (extra + "\n" + FOOT) if extra else FOOT,
             fontsize=8, color="#666666", ha="left", va="top")


def barlabels(ax, bars, fmt="%.0f", dy=8):
    for b in bars:
        ax.annotate(fmt % b.get_height(),
                    xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                    xytext=(0, dy), textcoords="offset points",
                    ha="center", va="bottom", fontsize=9.5, fontweight="bold")


# --------------------------------------------------------------------------
# 1. Where the ceiling actually is
# --------------------------------------------------------------------------
def ceiling():
    rows = {r["arm"]: float(r["MBps"]) for r in load("ceilings.csv")}
    items = [
        ("ssh alone, same connection\nand cipher, fed from memory", rows["ssh alone (pipe)"], GREEN),
        ("iperf3, one TCP stream", rows["iperf3 1 stream"], AMBER),
        ("rsync push\n(a real rsync receiver at the far end)", rows["rsync push"], ACCENT),
    ]
    labels = [i[0] for i in items]
    vals = [i[1] for i in items]
    cols = [i[2] for i in items]

    fig, ax = plt.subplots(figsize=(11, 5.0))
    y = range(len(items))
    bars = ax.barh(list(y), vals, color=cols, height=0.62, zorder=3)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels, fontsize=10)
    # room above the top bar for the band's caption
    ax.set_ylim(2.62, -1.05)
    ax.set_xlabel("MB/s, measured back to back in one thermal state")
    ax.set_xlim(0, 2200)
    ax.xaxis.set_major_locator(MultipleLocator(200))
    ax.grid(True, axis="x", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    for b, v in zip(bars, vals):
        ax.annotate("%.0f" % v, xy=(b.get_width(), b.get_y() + b.get_height() / 2),
                    xytext=(8, 0), textcoords="offset points",
                    va="center", fontsize=11, fontweight="bold")

    # the gap, as a band across every row rather than an arrow under them
    ax.axvspan(vals[2], vals[0], color=ACCENT, alpha=0.10, zorder=1)
    mid = (vals[0] + vals[2]) / 2
    ax.annotate("", xy=(vals[2], -0.62), xytext=(vals[0], -0.62),
                arrowprops=dict(arrowstyle="<->", color=ACCENT, lw=1.4))
    ax.annotate("−29%\nthe far-end rsync receiver, pegged at one core —\n"
                "nothing on the Windows side is above 65% of a core",
                xy=(mid, -0.70), ha="center", va="bottom", fontsize=9.5,
                color=INK)
    ax.set_title("The pipeline is not limited by this machine",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    footer(fig, "iperf3's single stream measures iperf3, not the link: ssh alone over the same "
                "connection sustains 1924 MB/s.", y=-0.12)
    save(fig, "windows-perf-xperf-ceiling")


# --------------------------------------------------------------------------
# 2. Two proofs that the receiver is the constraint
# --------------------------------------------------------------------------
def receiver():
    par = load("parallel-streams.csv")
    pre = {r["arm"]: float(r["MBps"]) for r in load("remote-prealloc.csv")}

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5.0),
                                   gridspec_kw={"width_ratios": [1.35, 1]})

    n = [int(r["streams"]) for r in par]
    agg = [float(r["aggMBps"]) for r in par]
    per = [float(r["perStream"]) for r in par]
    b = ax1.bar([str(i) for i in n], agg, 0.55, color=SEND, label="aggregate")
    barlabels(ax1, b)
    ax1.plot([str(i) for i in n], per, "o-", color=ACCENT, lw=2, ms=7,
             label="per stream")
    for xi, v in zip([str(i) for i in n], per):
        ax1.annotate("%.0f" % v, xy=(xi, v), xytext=(0, -16),
                     textcoords="offset points", ha="center", fontsize=9.5,
                     color=ACCENT, fontweight="bold")
    ax1.set_xlabel("simultaneous pushes, each to its own destination")
    ax1.set_ylabel("MB/s")
    ax1.set_ylim(0, 1900)
    ax1.yaxis.set_major_locator(MultipleLocator(200))
    ax1.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax1.set_axisbelow(True)
    ax1.legend(frameon=False, loc="lower right", fontsize=9.5)
    ax1.set_title("A second receiver buys +19%", fontsize=12,
                  fontweight="bold", loc="left", pad=10)

    names = ["destination deleted\nfirst (allocates 4 GB\nof tmpfs as it writes)",
             "destination already\nthere, overwritten"]
    vals = [pre["fresh"], pre["reuse"]]
    b2 = ax2.bar(names, vals, 0.5, color=[BEFORE, GREEN])
    barlabels(ax2, b2)
    ax2.set_ylabel("MB/s")
    ax2.set_ylim(0, 1900)
    ax2.yaxis.set_major_locator(MultipleLocator(200))
    ax2.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax2.set_axisbelow(True)
    ax2.tick_params(axis="x", labelsize=9)
    pct = 100 * (vals[1] - vals[0]) / vals[0]
    ax2.annotate("%+.1f%%" % pct, xy=(1, vals[1]), xytext=(0, 26),
                 textcoords="offset points", ha="center", fontsize=12,
                 fontweight="bold", color=GREEN)
    ax2.set_title("Making the receiver's job cheaper\nspeeds the whole transfer up",
                  fontsize=12, fontweight="bold", loc="left", pad=10)

    fig.suptitle("Both tests say the same thing: the far end sets the pace",
                 fontsize=14, fontweight="bold", x=0.008, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    footer(fig, "Right-hand panel uses --whole-file in both arms, so every byte crosses the wire "
                "either way and only the far side's page allocation differs.", y=-0.02)
    save(fig, "windows-perf-xperf-receiver")


# --------------------------------------------------------------------------
# 3. Nothing is saturated -- on either machine
# --------------------------------------------------------------------------
def busy():
    loc = load("thread-busy.csv")
    rem = load("remote-cpu.csv")

    # not sharey: the two panels name different roles (receiver vs sender) and a
    # shared y axis would show only the last panel's labels.  The x axis, which
    # is the one carrying the units, is identical on both.
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.2, 5.0))

    for ax, d, title in ((ax1, "push", "Push"), (ax2, "pull", "Pull")):
        lt = [r for r in loc if r["dir"] == d]
        rt = [r for r in rem if r["dir"] == d]
        names = [r["thread"] for r in lt] + [r["proc"] for r in rt]
        lows = [float(r["pctOfCore"]) for r in lt] + [float(r["pctOfCoreLow"]) for r in rt]
        highs = [float(r["pctOfCore"]) for r in lt] + [float(r["pctOfCoreHigh"]) for r in rt]
        cols = [SEND] * len(lt) + [ACCENT] * len(rt)
        y = range(len(names))
        ax.barh(list(y), highs, color=cols, height=0.6)
        # the remote figures are a range: show the floor as a lighter underlay
        for i, (lo, hi) in enumerate(zip(lows, highs)):
            if lo != hi:
                ax.barh(i, lo, color="white", height=0.6, alpha=0.45)
                ax.annotate("%.0f–%.0f%%" % (lo, hi), xy=(hi, i), xytext=(6, 0),
                            textcoords="offset points", va="center", fontsize=9.5,
                            fontweight="bold")
            else:
                ax.annotate("%.1f%%" % hi, xy=(hi, i), xytext=(6, 0),
                            textcoords="offset points", va="center", fontsize=9.5,
                            fontweight="bold")
        ax.set_yticks(list(y))
        ax.set_yticklabels(names, fontsize=9.5)
        ax.invert_yaxis()
        ax.set_xlim(0, 118)
        ax.xaxis.set_major_locator(MultipleLocator(20))
        ax.axvline(100, color=INK, lw=1.0, ls="--", alpha=0.6)
        ax.grid(True, axis="x", color="#e2e2e8", lw=0.8)
        ax.set_axisbelow(True)
        ax.set_xlabel("% of one core")
        ax.set_title(title, fontsize=12, fontweight="bold", loc="left", pad=8)

    for ax in (ax1, ax2):
        ax.annotate("one core", xy=(100, -0.72), ha="center", va="bottom",
                    fontsize=9, color=INK)
    fig.legend(handles=[plt.Rectangle((0, 0), 1, 1, color=SEND),
                        plt.Rectangle((0, 0), 1, 1, color=ACCENT)],
               labels=["this machine (per thread)", "the far end (per process)"],
               frameon=False, ncol=2, loc="upper left", bbox_to_anchor=(0.008, 0.925),
               fontsize=10)
    fig.suptitle("Only one thing in the whole pipeline runs out of CPU, and it is not ours",
                 fontsize=14, fontweight="bold", x=0.008, y=0.995, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    footer(fig, "Local figures from per-thread processor time; far-end figures sampled from "
                "/proc during the same transfers. Both panels share one axis.", y=-0.02)
    save(fig, "windows-perf-xperf-busy")


# --------------------------------------------------------------------------
# 4. Where each process's CPU goes
# --------------------------------------------------------------------------
def cpu_profile():
    rows = load("cpu-profile.csv")
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.8, 4.8), sharex=True)
    for ax, proc, col, title in (
            (ax1, "rsync.exe", SEND, "rsync.exe — 1.7 CPU-s per 4 GB push"),
            (ax2, "ssh.exe", GREEN, "ssh.exe — 4.1 CPU-s per 4 GB push")):
        rs = [r for r in rows if r["proc"] == proc]
        names = [r["item"] for r in rs]
        vals = [float(r["pct"]) for r in rs]
        y = range(len(rs))
        ax.barh(list(y), vals, color=col, height=0.6)
        for i, v in enumerate(vals):
            ax.annotate("%.1f%%" % v, xy=(v, i), xytext=(6, 0),
                        textcoords="offset points", va="center",
                        fontsize=9.5, fontweight="bold")
        ax.set_yticks(list(y))
        ax.set_yticklabels(names, fontsize=9.5)
        ax.invert_yaxis()
        ax.set_xlim(0, 30)
        ax.xaxis.set_major_locator(MultipleLocator(5))
        ax.grid(True, axis="x", color="#e2e2e8", lw=0.8)
        ax.set_axisbelow(True)
        ax.set_xlabel("% of that process's CPU samples")
        ax.set_title(title, fontsize=12, fontweight="bold", loc="left", pad=8)

    fig.suptitle("Neither process spends its time where you would guess",
                 fontsize=14, fontweight="bold", x=0.008, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    footer(fig, "xperf, -stackwalk Profile only, so these are CPU samples and not context switches. "
                "Reading the source is 53% of rsync's CPU inclusive; the cipher is only a fifth of ssh's.",
           y=-0.03)
    save(fig, "windows-perf-xperf-cpu")


# --------------------------------------------------------------------------
# 5. Spinning costs turbo
# --------------------------------------------------------------------------
def spin():
    rows = [r for r in load("ab-arms.csv")
            if r["arm"].startswith("SSH_SHMIO_SPIN") and r["dir"] == "pull"]
    rows.sort(key=lambda r: int(r["arm"].split("=")[1]))
    names = [r["arm"].split("=")[1] for r in rows]
    mbps = [float(r["MBps"]) for r in rows]
    clock = [float(r["clockPct"]) for r in rows]
    cpu = [float(r["cpuPerGB"]) for r in rows]

    fig, ax = plt.subplots(figsize=(11, 5.4))
    cols = [GREEN] + [ACCENT] * (len(rows) - 1)
    b = ax.bar(names, mbps, 0.5, color=cols)
    barlabels(ax, b)
    ax.set_xlabel("SSH_SHMIO_SPIN — PAUSE iterations before asking for a wake-up "
                  "(0 = the shipped behaviour)")
    ax.set_ylabel("MB/s, pull")
    ax.set_ylim(0, 1250)
    ax.yaxis.set_major_locator(MultipleLocator(200))
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)

    ax2 = ax.twinx()
    ax2.spines["right"].set_visible(True)
    ax2.plot(names, clock, "o--", color=PURPLE, lw=2, ms=8,
             label="effective clock (% of base)")
    for xi, v in zip(names, clock):
        # on a white plate: these sit over the bars
        ax2.annotate("%.0f%%" % v, xy=(xi, v), xytext=(0, 12),
                     textcoords="offset points", ha="center", fontsize=9.5,
                     color=PURPLE, fontweight="bold",
                     bbox=dict(boxstyle="round,pad=0.22", fc="white",
                               ec="none", alpha=0.88))
    ax2.set_ylabel("% Processor Performance", color=PURPLE)
    ax2.tick_params(axis="y", colors=PURPLE)
    ax2.set_ylim(100, 200)

    for xi, c in zip(names, cpu):
        ax.annotate("%.2f CPU-s/GB" % c, xy=(xi, 40), ha="center",
                    fontsize=8.5, color="#555555")

    ax.set_title("Six idle cores, and spinning still loses: on a 15 W part\n"
                 "a spinning core takes turbo from the threads doing the work",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    footer(fig, "Three interleaved rounds per arm, cooled to 62 °C before each round. "
                "Throughput and effective clock fall together — reverted, not shipped.", y=-0.06)
    save(fig, "windows-perf-xperf-spin")


# --------------------------------------------------------------------------
# 6. Three things that did not move
# --------------------------------------------------------------------------
def nulls():
    arms = load("ab-arms.csv")
    ring = sorted([r for r in arms if r["arm"].startswith("ring")],
                  key=lambda r: int(r["arm"][4:]))
    seq = sorted([r for r in arms if r["arm"].startswith("RSYNC_WIN32_WSEQSCAN")],
                 key=lambda r: r["arm"])

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.8), sharey=True,
                                   gridspec_kw={"width_ratios": [1.6, 1]})

    def kb(a):
        v = int(a[4:])
        return "%d MB" % (v // 1024) if v >= 1024 else "%d KB" % v
    n1 = [kb(r["arm"]) for r in ring]
    v1 = [float(r["MBps"]) for r in ring]
    b1 = ax1.bar(n1, v1, 0.5, color=BEFORE)
    barlabels(ax1, b1)
    ax1.set_xlabel("shared-memory ring size (push)")
    ax1.set_ylabel("MB/s")
    ax1.set_ylim(0, 1650)
    ax1.yaxis.set_major_locator(MultipleLocator(200))
    ax1.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax1.set_axisbelow(True)
    ax1.set_title("Ring size: flat across 64×", fontsize=12,
                  fontweight="bold", loc="left", pad=10)

    n2 = ["off\n(shipped)", "FILE_FLAG_\nSEQUENTIAL_SCAN"]
    v2 = [float(r["MBps"]) for r in seq]
    b2 = ax2.bar(n2, v2, 0.45, color=[BEFORE, AMBER])
    barlabels(ax2, b2)
    ax2.set_xlabel("hint on the pull's destination file")
    ax2.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax2.set_axisbelow(True)
    ax2.set_title("Sequential hint on writes: nothing", fontsize=12,
                  fontweight="bold", loc="left", pad=10)

    fig.suptitle("Negative results are results: neither of these is in the tree",
                 fontsize=14, fontweight="bold", x=0.008, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    footer(fig, "Both panels share one axis. A 16 MB ring gives rsync 12 ms of runway and changes "
                "nothing, which is another way of saying the ring was never the queue that mattered.",
           y=-0.03)
    save(fig, "windows-perf-xperf-nulls")


# --------------------------------------------------------------------------
# 7. The whole arc, 521ad8ad -> today
# --------------------------------------------------------------------------
def journey():
    # (label, MB/s, commit, era)  era 0 = 2.5 GbE, era 1 = Thunderbolt
    steps = [
        ("Windows ssh\nas shipped", 17, "before 521ad8ad", 0),
        ("pipe pumps,\nbundled ssh.exe", 249, "52d7cfd1", 0),
        ("same code,\n20 Gbit link", 326, "f800ace2", 1),
        ("stdout pump\n+ pselect()", 703, "fa2b317c", 1),
        ("socket pumps,\ncopies removed", 864, "e517f056\nd013a0e8", 1),
        ("AES-GCM via CNG,\nzero-copy send", 1000, "caca7c5e\nfc99ad80", 1),
        ("shared-memory\nring", 1021, "3e660145", 1),
        ("ISA-L AES-GCM,\nsequential source", 1362, "e9918e59", 1),
    ]
    labels = [s[0] for s in steps]
    vals = [s[1] for s in steps]
    ids = [s[2] for s in steps]
    eras = [s[3] for s in steps]
    x = list(range(len(steps)))

    fig, ax = plt.subplots(figsize=(14.6, 7.0))
    cols = ["#9aa7b4" if e == 0 else SEND for e in eras]
    cols[0] = "#c2c8cf"
    cols[-1] = GREEN
    bars = ax.bar(x, vals, 0.52, color=cols, zorder=3)

    ax.set_yscale("log")
    # headroom above the bars for the era titles and the two multipliers
    ax.set_ylim(9, 7000)
    ticks = [10, 20, 50, 100, 200, 500, 1000, 2000]
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_minor_locator(FixedLocator([]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, p: "%g" % v))
    ax.set_ylabel("MB/s, rsync push (Windows → Linux), log scale")
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8, zorder=0)
    ax.set_axisbelow(True)

    for b, v in zip(bars, vals):
        ax.annotate("%d" % v, xy=(b.get_x() + b.get_width() / 2, v),
                    xytext=(0, 6), textcoords="offset points", ha="center",
                    va="bottom", fontsize=11, fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8.5)
    for xi, cid in zip(x, ids):
        ax.annotate(cid, xy=(xi, -0.135), xycoords=("data", "axes fraction"),
                    ha="center", va="top", fontsize=8, color=HASH,
                    family="DejaVu Sans Mono")

    # link ceilings, per era.  Labels sit at the outer edge of each span so
    # they cannot land on a bar's value.
    ax.plot([-0.5, 1.5], [280, 280], ls="--", lw=1.4, color=ACCENT, zorder=4)
    ax.annotate("2.5 GbE line rate ≈ 280", xy=(-0.44, 185), ha="left",
                va="top", fontsize=9, color=ACCENT, fontweight="bold")
    ax.plot([1.5, 7.5], [1900, 1900], ls="--", lw=1.4, color=ACCENT, zorder=4)
    ax.annotate("Thunderbolt link ≈ 1900", xy=(1.6, 2020), ha="left",
                fontsize=9, color=ACCENT, fontweight="bold")
    ax.axvline(1.5, color="#b9b9c2", lw=1.1, ls=":", zorder=1)
    ax.annotate("2.5 GbE", xy=(0.5, 5200), ha="center", fontsize=10.5,
                color="#666666", style="italic")
    ax.annotate("Thunderbolt networking, 20 Gbit/s", xy=(4.5, 5200), ha="center",
                fontsize=10.5, color="#666666", style="italic")

    # multipliers, in the clear band between the bars and the era titles
    def span(x0, x1, y, text):
        ax.annotate("", xy=(x0, y), xytext=(x1, y),
                    arrowprops=dict(arrowstyle="<->", color=GREEN, lw=1.6))
        ax.annotate(text, xy=((x0 + x1) / 2, y), xytext=(0, 6),
                    textcoords="offset points", ha="center", va="bottom",
                    fontsize=10, color=GREEN, fontweight="bold")

    span(0, 1, 900, "15×\nand then the link, not the\ncode, was the limit")
    span(2, 7, 2900, "4.2× more, once a faster link showed there was room")

    ax.set_title("From 17 MB/s to 1362: the whole optimisation arc, 521ad8ad → e9918e59",
                 fontsize=15, fontweight="bold", loc="left", pad=16)
    footer(fig,
           "Each bar is what was measured at that commit, in that session; this laptop throttles, so "
           "only same-session comparisons are exact.\n"
           "The last bar is today's back-to-back figure. 80× overall, on a 15 W laptop doing AES on "
           "every byte — and the remaining gap to the\ndashed line is the far-end receiver, not this "
           "machine.", y=-0.155)
    save(fig, "windows-perf-journey")


ceiling()
receiver()
busy()
cpu_profile()
spin()
nulls()
journey()
