"""Charts for the Thunderbolt round of the rsync-windows speed work
(WINDOWS-PORT.md, "Moar Speed Pt. 3! Thunderbolt, Go!" onward), plus the
power-plan comparison.  Figures come from the commit messages f800ace2..HEAD
and the interleaved measurements taken with iperf3 3.21.

  tbcharts.py <outdir>
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

BEFORE = "#b8b8b8"
AFTER = "#1f77b4"
SEND = "#1f6fb4"
RECV = "#8fb8d8"
ACCENT = "#d62728"
GREEN = "#2ca02c"
PURPLE = "#7c3aed"
INK = "#1b1b1f"

FOOT = ("Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W)  ·  "
        "Linux: Ryzen 7 PRO 7840U  ·  Thunderbolt networking, MTU 65330, link 20 Gbit/s\n"
        "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  "
        "4 GB transfers, aes128-gcm, source file warm in the page cache  ·  "
        "© 2026 Max Vilimpoc")

HASH = "#7a4fd0"      # the colour commit ids are drawn in


def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=200,
                    bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", name)


def footer(fig, extra=None, y=-0.02):
    """y goes lower on charts that carry a row of commit ids under the axis."""
    fig.text(0.01, y, (extra + "\n" + FOOT) if extra else FOOT,
             fontsize=8, color="#666666", ha="left", va="top")


def commit_row(ax, xs, ids, y=-0.155):
    """Put the commit each bar group came from under its x label."""
    for x, cid in zip(xs, ids):
        ax.annotate(cid, xy=(x, y), xycoords=("data", "axes fraction"),
                    ha="center", va="top", fontsize=8.5, color=HASH,
                    family="DejaVu Sans Mono")


def barlabels(ax, bars, fmt="%.0f", dx=0.0, dy=8):
    for b in bars:
        ax.annotate(fmt % b.get_height(),
                    xy=(b.get_x() + b.get_width() / 2 + dx, b.get_height()),
                    xytext=(0, dy), textcoords="offset points",
                    ha="center", va="bottom", fontsize=9.5, fontweight="bold")


# --------------------------------------------------------------------------
# 1. The ssh client, step by step
# --------------------------------------------------------------------------
def ssh_ladder():
    steps = [
        ("shipped in\nv3.5.0-gf800ace2", 341, 223, "f800ace2"),
        ("stdout write pump\n+ native pselect()", 764, 573, "fa2b317c"),
        ("socket pumped\nboth ways", 1020, 930, "e517f056"),
        ("staging copies\nremoved", 1080, 1070, "d013a0e8"),
        ("AES-GCM via CNG,\nsshbuf kept", 1400, 1260, "caca7c5e"),
    ]
    labels = [s[0] for s in steps]
    send = [s[1] for s in steps]
    recv = [s[2] for s in steps]
    ids = [s[3] for s in steps]
    x = range(len(steps))
    w = 0.38

    fig, ax = plt.subplots(figsize=(11, 5.6))
    b1 = ax.bar([i - w / 2 for i in x], send, w, color=SEND, label="sending (4 GB out)")
    b2 = ax.bar([i + w / 2 for i in x], recv, w, color=RECV, label="receiving (4 GB in)")
    barlabels(ax, b1)
    barlabels(ax, b2)
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=9.5)
    ax.set_ylabel("MB/s, one ssh connection")
    ax.set_ylim(0, 1650)
    ax.yaxis.set_major_locator(MultipleLocator(250))
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, loc="upper left")
    ax.set_title("The bundled ssh.exe on a 20 Gbit link, one change at a time",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    ax.annotate("4.1× sending\n5.6× receiving", xy=(4, 1400), xytext=(3.35, 1520),
                fontsize=11, fontweight="bold", color=GREEN, ha="center")
    commit_row(ax, list(x), ids)
    footer(fig, "Each bar is the transfer phase of a 4 GB stream, measured with a sampling profiler attached. "
                "Commit ids under each step.\n"
                "Later commits improved this further without moving these two numbers: 2e45839b (arc4random off "
                "a kernel mutex), fc99ad80 (the encrypted\npacket handed to the socket thread, not copied), "
                "eacfa8d2 (pumps sleep between batches).", y=-0.115)
    save(fig, "windows-perf-tb-ssh-ladder")


# --------------------------------------------------------------------------
# 2. rsync end to end
# --------------------------------------------------------------------------
def rsync_endtoend():
    stages = ["v3.5.0-gf800ace2\n(2.5 GbE-era client)", "write pump\n+ pselect()",
              "socket pumps\n+ copies removed", "CNG, sshbuf,\nzero-copy send"]
    ids = ["f800ace2", "fa2b317c", "e517f056 + d013a0e8", "caca7c5e + fc99ad80"]
    push = [326, 703, 864, 1000]
    pull = [320, 676, 837, 980]
    x = range(len(stages))
    w = 0.38

    fig, ax = plt.subplots(figsize=(10.5, 5.4))
    b1 = ax.bar([i - w / 2 for i in x], push, w, color=SEND, label="push (Windows → Linux)")
    b2 = ax.bar([i + w / 2 for i in x], pull, w, color=RECV, label="pull (Linux → Windows)")
    barlabels(ax, b1)
    barlabels(ax, b2)
    ax.set_xticks(list(x))
    ax.set_xticklabels(stages, fontsize=9.5)
    ax.set_ylabel("MB/s, rsync end to end")
    ax.set_ylim(0, 1200)
    ax.yaxis.set_major_locator(MultipleLocator(200))
    ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    ax.legend(frameon=False, loc="upper left")
    ax.set_title("rsync over Thunderbolt: 3.1× pushing, 3.1× pulling",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    commit_row(ax, list(x), ids, y=-0.175)
    footer(fig, "One 4 GB file, --whole-file path, hash-verified on arrival each time. "
                "Commit ids under each step; all of them are in the ssh.exe the release ships,\n"
                "not in rsync.exe itself, except 8e1bd987 which trimmed rsync's own pipe layer.", y=-0.125)
    save(fig, "windows-perf-tb-rsync")


# --------------------------------------------------------------------------
# 3. What it costs in CPU
# --------------------------------------------------------------------------
def cpu_cost():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 5.2),
                                   gridspec_kw={"width_ratios": [1.25, 1]})

    labels = ["ssh.exe\naes128-gcm", "ssh.exe\nchacha20-poly1305"]
    before = [2.45, 2.90]
    after = [1.76, 1.25]
    x = range(len(labels))
    w = 0.36
    b1 = ax1.bar([i - w / 2 for i in x], before, w, color=BEFORE, label="pumps spinning")
    b2 = ax1.bar([i + w / 2 for i in x], after, w, color=AFTER, label="pumps sleeping between batches")
    barlabels(ax1, b1, "%.2f")
    barlabels(ax1, b2, "%.2f")
    ax1.set_xticks(list(x))
    ax1.set_xticklabels(labels, fontsize=10)
    ax1.set_ylabel("processor cores used")
    ax1.set_ylim(0, 3.5)
    ax1.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax1.set_axisbelow(True)
    ax1.legend(frameon=False, fontsize=9.5, loc="upper right")
    ax1.set_title("ssh.exe, same throughput for less CPU", fontsize=12,
                  fontweight="bold", loc="left")
    ax1.annotate("a core and a half\nof pure waste on a\nslow cipher", xy=(1.18, 2.9),
                 xytext=(1.30, 2.45), fontsize=9.5, color=ACCENT, ha="center")
    commit_row(ax1, [0, 1], ["caca7c5e → eacfa8d2"] * 2, y=-0.13)

    lab2 = ["rsync.exe\nduring a push"]
    b3 = ax2.bar([-w / 2], [89], w * 1.5, color=BEFORE, label="before")
    b4 = ax2.bar([w / 2 + 0.08], [80], w * 1.5, color=AFTER, label="after")
    barlabels(ax2, b3)
    barlabels(ax2, b4)
    ax2.set_xticks([0.04])
    ax2.set_xticklabels(lab2, fontsize=10)
    ax2.set_ylabel("% of one core")
    ax2.set_ylim(0, 120)
    ax2.yaxis.set_major_locator(MultipleLocator(25))
    ax2.grid(True, axis="y", color="#e2e2e8", lw=0.8)
    ax2.set_axisbelow(True)
    ax2.legend(frameon=False, fontsize=9.5, loc="upper right")
    ax2.set_title("stop re-signalling pipe state,\nstop re-asking what an fd is",
                  fontsize=12, fontweight="bold", loc="left")
    commit_row(ax2, [0.04], ["eacfa8d2 → 8e1bd987"], y=-0.13)

    fig.subplots_adjust(wspace=0.28)
    footer(fig, "Processor time over a 4 GB transfer, divided by its wall clock. "
                "Commit ids under each pair: left is in the bundled ssh.exe,\nright is rsync.exe's own "
                "Windows pipe layer (win32/win32io.c).", y=-0.10)
    save(fig, "windows-perf-tb-cpu")


# --------------------------------------------------------------------------
# 4. What is left, and where it goes
# --------------------------------------------------------------------------
def budget():
    rows = [
        ("the link itself\niperf3, 4 streams", 1894, "#2ca02c"),
        ("one TCP stream\niperf3, 1 stream", 1437, "#5aa469"),
        ("ssh, stdin from a file\nno pipe in the way", 1275, SEND),
        ("ssh, stdin from a pipe\n(no rsync involved)", 1045, "#e08b4a"),
        ("rsync push\npipe + rsync protocol", 945, ACCENT),
    ]
    labels = [r[0] for r in rows]
    vals = [r[1] for r in rows]
    cols = [r[2] for r in rows]

    fig, ax = plt.subplots(figsize=(10.5, 5.6))
    bars = ax.barh(range(len(rows)), vals, color=cols, height=0.62)
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels(labels, fontsize=10)
    ax.invert_yaxis()
    ax.set_xlabel("MB/s")
    ax.set_xlim(0, 2150)
    ax.xaxis.set_major_locator(MultipleLocator(250))
    ax.grid(True, axis="x", color="#e2e2e8", lw=0.8)
    ax.set_axisbelow(True)
    for i, b in enumerate(bars):
        ax.annotate("%d" % vals[i], xy=(b.get_width(), b.get_y() + b.get_height() / 2),
                    xytext=(7, 0), textcoords="offset points", va="center",
                    fontsize=10.5, fontweight="bold")
    ax.annotate("", xy=(1045, 3.42), xytext=(1275, 3.42),
                arrowprops=dict(arrowstyle="<->", color=ACCENT, lw=1.6))
    ax.annotate("the pipe between rsync and ssh: −18%",
                xy=(1160, 3.60), ha="center", fontsize=10, color=ACCENT, fontweight="bold")
    ax.annotate("rsync's own protocol work: −10%",
                xy=(995, 4.46), ha="center", fontsize=10, color="#8a2a2a")
    ax.set_title("Where a push's throughput goes, measured back to back",
                 fontsize=14, fontweight="bold", loc="left", pad=14)
    footer(fig, "All five measured in one interleaved run at a8664bae, because this laptop throttles: the "
                "same push can read 25% slower an hour later.\nThe two ssh rows differ only in what feeds "
                "stdin, so the gap between them is the cost of the pipe and nothing else.")
    save(fig, "windows-perf-tb-budget")


# --------------------------------------------------------------------------
# 5. Power plan: Balanced vs Ultimate Performance
# --------------------------------------------------------------------------
def power_plan():
    rsync_bal = [966, 959, 980, 1004]
    rsync_ult = [958, 950, 932, 946]
    iperf_bal = [1418, 1422, 1416, 1518]
    iperf_ult = [1414, 1414, 1517, 1516]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 5.4))

    for ax, bal, ult, title, ymax in (
            (ax1, rsync_bal, rsync_ult, "rsync push, 2 GB  (4 runs each)", 1300),
            (ax2, iperf_bal, iperf_ult, "iperf3, one stream, 15 s  (4 runs each)", 1900)):
        mb, mu = sum(bal) / len(bal), sum(ult) / len(ult)
        b = ax.bar([0], [mb], 0.5, color=AFTER, label="Balanced")
        u = ax.bar([1], [mu], 0.5, color=PURPLE, label="Ultimate Performance")
        # individual runs, nudged off centre so they never sit on the label
        ax.scatter([-0.21] * len(bal), bal, color=INK, zorder=5, s=24)
        ax.scatter([0.79] * len(ult), ult, color=INK, zorder=5, s=24)
        for xc, m in ((0, mb), (1, mu)):
            ax.annotate("%.0f" % m, xy=(xc, m), xytext=(0, 10),
                        textcoords="offset points", ha="center",
                        fontsize=11, fontweight="bold")
        ax.set_xticks([0, 1])
        ax.set_xticklabels(["Balanced", "Ultimate\nPerformance"], fontsize=10.5)
        ax.set_ylabel("MB/s")
        ax.set_ylim(0, ymax)
        ax.grid(True, axis="y", color="#e2e2e8", lw=0.8)
        ax.set_axisbelow(True)
        ax.set_title(title, fontsize=12, fontweight="bold", loc="left")
        spread = (max(bal + ult) - min(bal + ult)) / mb * 100
        pct = 100 * (mu - mb) / mb
        ax.annotate("%+.1f%%\n(run-to-run spread is ±%.0f%%)" % (pct, spread / 2),
                    xy=(1, mu), xytext=(0, 34), textcoords="offset points",
                    ha="center", fontsize=10.5, fontweight="bold",
                    color=ACCENT if pct < -2 else "#666666")

    handles, labels = ax1.get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, fontsize=10, ncol=2,
               loc="lower center", bbox_to_anchor=(0.5, -0.035))
    fig.suptitle("Ultimate Performance buys nothing here (dots are the individual runs)",
                 x=0.012, ha="left", fontsize=14, fontweight="bold", y=1.02)
    fig.subplots_adjust(wspace=0.26)
    fig.text(0.01, -0.075,
             "Runs alternated Balanced/Ultimate/Ultimate/Balanced so thermal drift cannot favour either. "
             "Ultimate Performance sets PCIe ASPM to Off\n(from Maximum power savings) and minimum processor "
             "state to 100% (from 5%); the effective clock came out 189% vs 187% of base either way.\n"
             "No code changed between the two: both columns are rsync 3.5.0 Windows port at a8664bae, only the "
             "Windows power plan differs.\n"
             "Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W)  ·  Linux: Ryzen 7 PRO 7840U  ·  "
             "Thunderbolt networking, MTU 65330, link 20 Gbit/s\n"
             "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  aes128-gcm, source file warm in the "
             "page cache  ·  © 2026 Max Vilimpoc",
             fontsize=8, color="#666666", ha="left", va="top")
    save(fig, "windows-perf-tb-power-plan")


ssh_ladder()
rsync_endtoend()
cpu_cost()
budget()
power_plan()
