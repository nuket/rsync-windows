"""Charts for the shared-memory hop between rsync.exe and ssh.exe
(BUILD-CMAKE.md, "The hop to ssh"; commit 3e660145 against a8664bae).

Figures come from scratchpad/perflog.csv: the standalone parent-to-child
transport benchmark, and four interleaved 4.3 GB pushes per transport with
the processor time of both processes sampled.

  shmcharts.py <outdir>
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

OUT = sys.argv[1]

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "svg.fonttype": "none",
})

PIPE = "#b8b8b8"       # the kernel pipe: the thing being replaced
RING = "#1f77b4"       # the shared-memory ring
GREEN = "#2ca02c"
INK = "#1b1b1f"
HASH = "#7a4fd0"

MACHINE = ("Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W)  ·  "
           "Linux: Ryzen 7 PRO 7840U  ·  Thunderbolt networking, MTU 65330, link 20 Gbit/s\n"
           "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  "
           "kernel pipe = a8664bae, shared-memory ring = 3e660145  ·  "
           "© 2026 Max Vilimpoc")
TRANSFER = ("4.3 GB transfers, aes128-gcm, source file warm in the page cache  ·  ")

# The standalone benchmark never leaves the laptop, so its footer says so and
# leaves out the Linux box and the link entirely.
BENCH = ("Both processes on the Windows machine: Dell Latitude 7490, i5-8350U "
         "(4 cores / 8 threads, 15 W)  ·  no network, no disk\n"
         "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  "
         "win32/win32shmpipe.c as committed in 3e660145  ·  "
         "© 2026 Max Vilimpoc")
FOOT = MACHINE.replace("kernel pipe = ", TRANSFER + "kernel pipe = ")

# the four interleaved runs of each, alternating pipe/ring/pipe/ring/...
PUSH_PIPE = [1048, 935, 955, 967]
PUSH_RING = [1067, 1000, 1007, 1009]
CPU_GB_PIPE = [2.49, 2.83, 2.88, 2.85]
CPU_GB_RING = [2.14, 2.32, 2.17, 2.13]
RSYNC_PIPE = [3.38, 3.75, 4.00, 3.77]
RSYNC_RING = [2.48, 2.78, 2.58, 2.45]
SSH_PIPE = [7.31, 8.39, 8.39, 8.45]
SSH_RING = [6.70, 7.19, 6.72, 6.69]


def mean(xs):
    return sum(xs) / len(xs)


def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=200,
                    bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", name)


def footer(fig, extra=None, y=-0.02, foot=FOOT):
    fig.text(0.01, y, (extra + "\n" + foot) if extra else foot,
             fontsize=8, color="#666666", ha="left", va="top")


def barlabels(ax, bars, fmt="%.0f", dy=8):
    for b in bars:
        ax.annotate(fmt % b.get_height(),
                    xy=(b.get_x() + b.get_width() / 2, b.get_height()),
                    xytext=(0, dy), textcoords="offset points",
                    ha="center", va="bottom", fontsize=9.5, fontweight="bold")


def dots(ax, x, vals, w):
    """The individual runs, on the left half of their bar so they do not sit
    under the value label."""
    ax.scatter([x - 0.3 * w] * len(vals), vals, color=INK, s=22, zorder=5)


# --------------------------------------------------------------------------
# 1. The result: what a push costs, with and without the pipe
# --------------------------------------------------------------------------
def result():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 5.4),
                                   gridspec_kw={"width_ratios": [1.35, 1]})

    # -- throughput, both directions
    w = 0.36
    push = (mean(PUSH_PIPE), mean(PUSH_RING))
    pull = (884.0, 913.0)
    b1 = ax1.bar([0 - w / 2, 1 - w / 2], [push[0], pull[0]], w, color=PIPE,
                 label="kernel pipe  (a8664bae)")
    b2 = ax1.bar([0 + w / 2, 1 + w / 2], [push[1], pull[1]], w, color=RING,
                 label="shared-memory ring  (3e660145)")
    barlabels(ax1, b1)
    barlabels(ax1, b2)
    dots(ax1, -w / 2, PUSH_PIPE, w)
    dots(ax1, w / 2, PUSH_RING, w)
    ax1.set_xticks([0, 1])
    ax1.set_xticklabels(["push (Windows → Linux)\n4 runs each, alternating",
                         "pull (Linux → Windows)\none run each"], fontsize=10)
    ax1.set_ylabel("MB/s, rsync end to end")
    ax1.set_ylim(0, 1500)
    ax1.yaxis.set_major_locator(MultipleLocator(200))
    ax1.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax1.set_axisbelow(True)
    ax1.legend(frameon=False, fontsize=10, loc="upper left")
    ax1.set_title("Throughput moves a little …", fontsize=12,
                  fontweight="bold", loc="left")
    for xc, val, pct in ((w / 2, push[1], "+4.6%"), (1 + w / 2, pull[1], "+3.3%")):
        ax1.annotate(pct, xy=(xc, val), xytext=(0, 30),
                     textcoords="offset points", ha="center", fontsize=10.5,
                     fontweight="bold", color=GREEN)

    # -- CPU per GB
    cg = (mean(CPU_GB_PIPE), mean(CPU_GB_RING))
    b3 = ax2.bar([0], [cg[0]], 0.5, color=PIPE)
    b4 = ax2.bar([1], [cg[1]], 0.5, color=RING)
    barlabels(ax2, b3, "%.2f")
    barlabels(ax2, b4, "%.2f")
    dots(ax2, 0, CPU_GB_PIPE, 0.5)
    dots(ax2, 1, CPU_GB_RING, 0.5)
    ax2.set_xticks([0, 1])
    ax2.set_xticklabels(["kernel pipe", "shared-memory\nring"], fontsize=10)
    ax2.set_ylabel("processor seconds per GB pushed")
    ax2.set_ylim(0, 3.9)
    ax2.yaxis.set_major_locator(MultipleLocator(0.5))
    ax2.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax2.set_axisbelow(True)
    ax2.set_title("… the work does not", fontsize=12, fontweight="bold", loc="left")
    ax2.annotate("−21%", xy=(1, cg[1]), xytext=(0, 32), textcoords="offset points",
                 ha="center", fontsize=12, fontweight="bold", color=GREEN)

    fig.suptitle("rsync.exe and ssh.exe share memory instead of a pipe: a fifth of a push's work, gone",
                 x=0.012, ha="left", fontsize=14, fontweight="bold", y=1.02)
    fig.subplots_adjust(wspace=0.3)
    footer(fig, "Both processes' processor time over the same 4.3 GB file, divided by the bytes moved; dots are "
                "the individual runs, which alternated\ntransports so the laptop's thermal drift lands on both. "
                "One binary either way — RSYNC_WIN32_NO_SHMPIPE=1 keeps the pipes.", y=-0.10)
    save(fig, "windows-perf-tb-shmpipe")


# --------------------------------------------------------------------------
# 2. Which process stopped working
# --------------------------------------------------------------------------
def per_process():
    labels = ["rsync.exe", "ssh.exe"]
    pipe = [mean(RSYNC_PIPE), mean(SSH_PIPE)]
    ring = [mean(RSYNC_RING), mean(SSH_RING)]
    runs_pipe = [RSYNC_PIPE, SSH_PIPE]
    runs_ring = [RSYNC_RING, SSH_RING]
    x = range(len(labels))
    w = 0.38

    fig, ax = plt.subplots(figsize=(11.5, 5.4))
    b1 = ax.bar([i - w / 2 for i in x], pipe, w, color=PIPE, label="kernel pipe")
    b2 = ax.bar([i + w / 2 for i in x], ring, w, color=RING, label="shared-memory ring")
    barlabels(ax, b1, "%.2f")
    barlabels(ax, b2, "%.2f")
    for i in x:
        dots(ax, i - w / 2, runs_pipe[i], w)
        dots(ax, i + w / 2, runs_ring[i], w)
    for i, pct in ((0, "−31%"), (1, "−16%")):
        ax.annotate(pct, xy=(i + w / 2, ring[i]), xytext=(0, 30),
                    textcoords="offset points", ha="center", fontsize=11,
                    fontweight="bold", color=GREEN)
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylabel("processor seconds per 4.3 GB push")
    ax.set_ylim(0, 10.5)
    ax.yaxis.set_major_locator(MultipleLocator(1))
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, fontsize=10, loc="upper left")
    ax.set_title("Two copies per byte fewer, and no system call per chunk",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    for i in x:
        ax.annotate("a8664bae → 3e660145", xy=(i, -0.085),
                    xycoords=("data", "axes fraction"), ha="center", va="top",
                    fontsize=8.5, color=HASH, family="DejaVu Sans Mono")
    footer(fig, "Processor time of each process over the same 4.3 GB push, mean of four interleaved runs; dots are "
                "the runs.\nrsync stops copying into the kernel and calling WriteFile per chunk; ssh stops copying "
                "back out of it, and the pump thread that\nread its stdin gives way to a notifier thread that moves "
                "no bytes and only runs when the main loop is about to block.", y=-0.075)
    save(fig, "windows-perf-tb-shmpipe-cpu")


# --------------------------------------------------------------------------
# 3. The transport on its own
# --------------------------------------------------------------------------
def transport():
    chunks = ["32 KB\n(what rsync writes)", "64 KB\n(iobuf.out)", "256 KB"]
    pipe = [6.007, 5.706, 9.299]
    ring = [22.652, 22.403, 21.488]
    x = range(len(chunks))
    w = 0.34

    fig, ax = plt.subplots(figsize=(11.5, 5.2))
    b1 = ax.bar([i - w / 2 for i in x], pipe, w, color=PIPE,
                label="anonymous pipe, 1 MB buffer")
    b2 = ax.bar([i + w / 2 for i in x], ring, w, color=RING,
                label="shared-memory ring, 1 MB")
    barlabels(ax, b1, "%.1f")
    barlabels(ax, b2, "%.1f")
    ax.set_xticks(list(x))
    ax.set_xticklabels(chunks, fontsize=10)
    ax.set_ylabel("GB/s, parent to child on one machine")
    ax.set_ylim(0, 32)
    ax.yaxis.set_major_locator(MultipleLocator(5))
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, fontsize=10, loc="upper left",
              bbox_to_anchor=(0.30, 1.0))
    ax.set_title("Why it was worth trying: the hop on its own, between two processes on one machine",
                 fontsize=13.5, fontweight="bold", loc="left", pad=14)
    ax.annotate("3.8×", xy=(0 + w / 2, ring[0]), xytext=(0, 30),
                textcoords="offset points", ha="center", fontsize=12,
                fontweight="bold", color=GREEN)
    footer(fig, "Nothing here crosses a network. A parent and a child on the Windows laptop, the parent writing bytes "
                "and the child throwing them\naway: the hop between two local processes and nothing else — no ssh, no "
                "cipher, no link, no disk. The pipe cares about the chunk\nsize and the ring does not. At 1.5 GB/s the "
                "pipe costs about 26% of a core in transport alone and the ring about 7%, which is the\nsaving the "
                "measured push turns into.", foot=BENCH)
    save(fig, "windows-perf-tb-shmpipe-transport")


result()
per_process()
transport()
