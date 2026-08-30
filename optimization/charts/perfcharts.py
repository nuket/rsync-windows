"""Performance charts for the rsync-windows speed work, from the figures in
the commit messages (ea999814..HEAD), PERF.txt and WINDOWS-PORT.md.
Writes windows-perf-*.svg and .png into the directory given as argv[1]."""
import sys, os
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
    "svg.fonttype": "none",       # keep text as text in the SVG
})
BEFORE = "#b8b8b8"
AFTER = "#1f77b4"
ACCENT = "#d62728"
GREEN = "#2ca02c"
FOOT = ("Windows: i5-8350U laptop, Realtek RTL8156 USB 2.5GbE, Intel I219-LM 1GbE  ·  "
        "Linux: Ryzen 9 7950X  ·  rsync 3.5.0 Windows port, github.com/nuket/rsync-windows")

def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, f"{name}.{ext}"), dpi=200, bbox_inches="tight",
                    facecolor="white")
    plt.close(fig)
    print("wrote", name)

def footer(fig, extra=None):
    text = FOOT if not extra else extra + "\n" + FOOT
    fig.text(0.01, -0.02, text, fontsize=8, color="#666666", ha="left", va="top")

# ---------------------------------------------------------------------------
# 1. Transfer rates, before / after, per scenario
# ---------------------------------------------------------------------------
def transfer_rates():
    rows = [
        # label, before, after, note
        ("Linux → Windows, 1 GbE",                         7.0, 112.5, "wire rate"),
        ("Linux → Windows, 2.5 GbE, AES-GCM",              7.0, 273.9, "wire rate"),
        ("Linux → Windows, 2.5 GbE, ssh default chacha20", 7.0, 155.7, "cipher-bound: no AES-NI for chacha20"),
        ("Windows → Linux, pulled from Linux, 2.5 GbE",  180.0, 266.0, "NIC driver update + write pump"),
        ("Windows → Linux, pushed from Windows, 2.5 GbE",  17.0, 249.0, "bundled ssh.exe"),
    ]
    fig, ax = plt.subplots(figsize=(11, 5.6))
    y = list(range(len(rows)))[::-1]
    h = 0.36
    for yi, (label, b, a, note) in zip(y, rows):
        ax.barh(yi + h / 2, b, h, color=BEFORE, label="before" if yi == y[0] else None)
        ax.barh(yi - h / 2, a, h, color=AFTER, label="after" if yi == y[0] else None)
        ax.text(b + 3, yi + h / 2, f"{b:g} MB/s", va="center", fontsize=9, color="#555555")
        ax.text(a + 3, yi - h / 2, f"{a:g} MB/s   ×{a / b:.0f}" if a / b >= 2 else f"{a:g} MB/s   ×{a / b:.1f}",
                va="center", fontsize=9, color=AFTER, fontweight="bold")
        ax.text(a + 3, yi - h / 2 - 0.2, note, va="top", fontsize=8, color="#777777", style="italic")
    ax.set_yticks(y)
    ax.set_yticklabels([r[0] for r in rows])
    ax.set_xlim(0, 360)
    ax.xaxis.set_major_locator(MultipleLocator(50))
    ax.set_xlabel("throughput of a bulk transfer, MB/s")
    for x, txt in ((118, "1 GbE wire"), (291, "2.5 GbE wire")):
        ax.axvline(x, color=ACCENT, linestyle=(0, (4, 3)), linewidth=1)
        ax.text(x, len(rows) - 0.35, txt, color=ACCENT, fontsize=8, ha="center", va="bottom")
    ax.set_ylim(-0.7, len(rows) - 0.2)
    ax.legend(loc="lower right", frameon=False)
    ax.set_title("rsync between a Linux workstation and a Windows laptop: before and after the port's speed work",
                 loc="left", fontsize=12, fontweight="bold", pad=28)
    ax.text(0, 1.02, "Measured with 1-2 GB files; the job that started it was a ~90 GB data set. "
            "\"Before\" is commit ea999814's parent (27 Aug 2026), \"after\" is HEAD.",
            transform=ax.transAxes, fontsize=9, color="#555555", va="bottom")
    footer(fig)
    save(fig, "windows-perf-transfer-rates")

# ---------------------------------------------------------------------------
# 2. Timeline: throughput per milestone, three directions
# ---------------------------------------------------------------------------
def timeline():
    steps = [
        ("before\n(PERF.txt)", ""),
        ("pipe read pump", "ea999814"),
        ("xxHash bundled", "87d43e59"),
        ("pipe write pump", "007a22b5"),
        ("NIC driver update", "(no commit)"),
        ("bundled ssh.exe", "52d7cfd1"),
        ("delta search", "e6b8481b"),
        ("SIMD checksums", "2efaee77"),
    ]
    series = [
        ("Linux → Windows push (AES-GCM)",          [7, 238.2, 273.9, 273.9, 273.9, 273.9, 273.9, 273.9], AFTER, "o"),
        ("Windows → Linux, pulled from Linux",     [180, 180, 180, 176, 266, 266, 266, 266], GREEN, "s"),
        ("Windows → Linux, pushed from Windows",   [17, 17, 17, 17, 17, 249, 249, 249], "#ff7f0e", "^"),
    ]
    fig, ax = plt.subplots(figsize=(11, 5.6))
    x = list(range(len(steps)))
    for label, ys, color, marker in series:
        ax.step(x, ys, where="post", color=color, linewidth=2.2, alpha=0.9)
        ax.plot(x, ys, linestyle="none", marker=marker, color=color, markersize=7, label=label)
        ax.text(x[-1] + 0.12, ys[-1], f"{ys[-1]:g}", color=color, va="center", fontsize=9, fontweight="bold")
    ax.axhline(291, color=ACCENT, linestyle=(0, (4, 3)), linewidth=1)
    ax.text(0, 294, "2.5 GbE wire, 291 MB/s raw TCP", color=ACCENT, fontsize=8, va="bottom")
    ax.set_xticks(x)
    ax.set_xticklabels([f"{a}\n{b}" for a, b in steps], fontsize=8.5)
    ax.set_ylabel("MB/s over 2.5 GbE")
    ax.set_ylim(0, 320)
    ax.set_xlim(-0.3, len(steps) - 0.4)
    ax.grid(axis="y", color="#eeeeee")
    ax.legend(loc="lower right", bbox_to_anchor=(0.995, 0.10), frameon=False, fontsize=9)
    ax.set_title("Where each change moved the needle", loc="left", fontsize=12, fontweight="bold", pad=28)
    ax.text(0, 1.02, "Bulk transfer rate over the 2.5 GbE link after each commit, all three directions. "
            "Flat segments are changes that fixed something else (CPU use, the delta path).",
            transform=ax.transAxes, fontsize=9, color="#555555", va="bottom")
    # annotate the two CPU-only wins
    ax.annotate("write pump: rsync.exe 100% → 10-25% of a core,\nrate unchanged (NIC-bound)",
                xy=(3, 176), xytext=(2.2, 110), fontsize=8, color="#555555", ha="center",
                arrowprops=dict(arrowstyle="->", color="#999999"))
    ax.text(6.5, 175, "delta search + SIMD:\nbulk rate unchanged;\nchanged data 106 s → 6.7 s\n(see the delta chart)",
            fontsize=8, color="#555555", ha="center", va="center")
    footer(fig)
    save(fig, "windows-perf-timeline")

# ---------------------------------------------------------------------------
# 3. Hash rates
# ---------------------------------------------------------------------------
def hash_rates():
    # One y range for both panels, so the two hashes can be read against each
    # other: xxh128 is the faster of the two by a wide margin, and that is
    # invisible when each panel is scaled to its own maximum.
    YMAX = 30
    X86 = "#9ecae1"    # x86 first, then x64: the older architecture leads
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.8), sharey=True,
                                   gridspec_kw={"width_ratios": [4, 2.4]})
    impls = ["scalar\n(before)", "SSE2", "SSSE3", "AVX2\n(chosen)"]
    x86 = [2.0, 4.2, 6.6, 11.9]
    x64 = [3.3, 4.3, 7.0, 12.1]
    xs = list(range(len(impls)))
    w = 0.38
    b1 = ax1.bar([i - w / 2 for i in xs], x86, w, color=X86, label="x86 build")
    b2 = ax1.bar([i + w / 2 for i in xs], x64, w, color=AFTER, label="x64 build")
    for bars in (b1, b2):
        for r in bars:
            ax1.text(r.get_x() + r.get_width() / 2, r.get_height() + 0.4, f"{r.get_height():g}",
                     ha="center", va="bottom", fontsize=9)
    ax1.set_xticks(xs)
    ax1.set_xticklabels(impls)
    ax1.set_ylabel("GB/s, one core (same scale on both panels)")
    ax1.set_ylim(0, YMAX)
    ax1.set_title("Block checksum (get_checksum1)\nscalar C before, SIMD chosen at runtime after",
                  loc="left", fontsize=11, fontweight="bold")
    ax1.legend(frameon=False, loc="upper left")
    ax1.text(3, 14.2, "×3.7", ha="center", color=AFTER, fontweight="bold")

    labels = ["baseline build\n(SSE2, before)", "AVX2 build\n(after)"]
    x86h = [14.8, 22.8]
    x64h = [15.7, 25.1]
    xs2 = [0, 1]
    c1 = ax2.bar([i - w / 2 for i in xs2], x86h, w, color=X86)
    c2 = ax2.bar([i + w / 2 for i in xs2], x64h, w, color=AFTER)
    for bars in (c1, c2):
        for r in bars:
            ax2.text(r.get_x() + r.get_width() / 2, r.get_height() + 0.4, f"{r.get_height():g}",
                     ha="center", va="bottom", fontsize=9)
    ax2.set_xticks(xs2)
    ax2.set_xticklabels(labels)
    ax2.set_ylim(0, YMAX)
    ax2.tick_params(axis="y", labelleft=True)
    ax2.set_title("xxh128 (file and block digests)\nsecond copy of xxHash built for AVX2",
                  loc="left", fontsize=11, fontweight="bold")
    ax2.text(1, 27.4, "×1.6", ha="center", color=AFTER, fontweight="bold")
    footer(fig, "Measured by the t_win32checksum helper on a 1 MB buffer, quiet machine, clean build (b0ad68fc). "
                "xxHash's own MSVC dispatcher was 2.5× slower than the baseline and was not used.")
    save(fig, "windows-perf-hash-rates")

# ---------------------------------------------------------------------------
# 4. Delta sync
# ---------------------------------------------------------------------------
def delta_sync():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.6))
    def pair(ax, title, before, after, unit, after2=None, after2_label=None):
        labels = ["before", "after\ndelta search"]
        vals = [before, after]
        colors = [BEFORE, AFTER]
        if after2 is not None:
            labels.append(after2_label)
            vals.append(after2)
            colors.append("#9ecae1")
        bars = ax.bar(labels, vals, color=colors, width=0.55)
        for r, v in zip(bars, vals):
            ax.text(r.get_x() + r.get_width() / 2, v + max(vals) * 0.015, f"{v:g} {unit}",
                    ha="center", va="bottom", fontsize=10)
        ax.set_ylim(0, max(vals) * 1.18)
        ax.set_ylabel(unit == "s" and "seconds (lower is better)" or unit)
        ax.set_title(title, loc="left", fontsize=11, fontweight="bold")
        ax.text(1, after + max(vals) * 0.09, f"×{before / after:.0f} faster", ha="center",
                color=AFTER, fontweight="bold")
    pair(ax1, "2.2 GB file, 78% of blocks changed,\npulled Windows → Linux over 2.5 GbE",
         106, 6.7, "s")
    ax1.text(0.5, 0.55, "~21 MB/s, one core pinned\n→ line rate, sender ~25% of a core",
             transform=ax1.transAxes, fontsize=9, color="#555555", ha="center")
    pair(ax2, "512 MB slice, local delta\n(both ends on the laptop)", 16.3, 1.1, "s",
         after2=1.05, after2_label="+ SIMD\nchecksums")
    footer(fig, "Delta transfer of a file edited in place: rsync's sender rolled its weak checksum byte by byte through every "
                "changed block; now it probes the next aligned blocks first and prefetches its lookups.")
    save(fig, "windows-perf-delta-sync")

transfer_rates()
timeline()
hash_rates()
delta_sync()
