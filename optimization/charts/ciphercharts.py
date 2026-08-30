"""Two charts for the AES-128-GCM library comparison:

  windows-perf-tb-cipher-libs     what each library can do, and what it
                                  actually sustains once the chip is hot
  windows-perf-tb-cipher-thermal  the two minutes, second by second

  ciphercharts.py <datadir> <outdir>
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

DATA, OUT = sys.argv[1], sys.argv[2]

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 11,
    "axes.spines.top": False, "axes.spines.right": False,
    "svg.fonttype": "none",
})

FOOT = ("Windows: Dell Latitude 7490, i5-8350U (4 cores / 8 threads, 15 W)  ·  "
        "Linux: Ryzen 7 PRO 7840U  ·  Thunderbolt networking, MTU 65330, link 20 Gbit/s\n"
        "AES-128-GCM as SSH uses it: one complete GCM message per 32 KB packet, fresh IV, "
        "4 bytes of AAD, 16-byte tag, one thread\n"
        "rsync 3.5.0 Windows port, github.com/nuket/rsync-windows  ·  © 2026 Max Vilimpoc")

LIBS = [
    ("ISA-L",     "isal",     "#1f6fb4"),
    ("OpenSSL 3", "openssl3", "#2ca02c"),
    ("CNG",       "cng",      "#e08b4a"),
    ("LibreSSL",  "libressl", "#d62728"),
]

# offline, best of three interleaved rounds, 32 KB messages
OFFLINE = {"isal": 5347, "openssl3": 5265, "cng": 3452, "libressl": 2247}


def save(fig, name):
    for ext in ("svg", "png"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=200,
                    bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", name)


def footer(fig, extra=None, y=-0.02):
    fig.text(0.01, y, (extra + "\n" + FOOT) if extra else FOOT,
             fontsize=8, color="#666666", ha="left", va="top")


def read_thr(tag):
    out = []
    for line in open(os.path.join(DATA, "net-%s-thr.csv" % tag)):
        f = line.strip().split(",")
        if len(f) == 2:
            try:
                out.append((float(f[0]), float(f[1])))
            except ValueError:
                pass
    return out


def read_env(tag):
    import csv
    rows = list(csv.DictReader(open(os.path.join(DATA, "net-%s-env.csv" % tag))))
    return [(float(r["t"]), float(r["clock"]), float(r["cpu"]),
             float(r["temp"]), float(r["fan"])) for r in rows]


def stats(tag):
    v = [x[1] for x in read_thr(tag)]
    n = max(1, len(v) // 4)
    first = sum(v[:n]) / n
    last = sum(v[-n:]) / n
    return {"mean": sum(v) / len(v), "first": first, "last": last,
            "decay": 100 * (1 - last / first)}


# --------------------------------------------------------------- chart 1
def libs_chart():
    fig, ax = plt.subplots(1, 3, figsize=(13.5, 5.2),
                           gridspec_kw={"width_ratios": [1.15, 1.15, 1]})

    names = [l[0] for l in LIBS]
    cols = [l[2] for l in LIBS]
    x = range(len(LIBS))

    off = [OFFLINE[l[1]] for l in LIBS]
    b = ax[0].bar(x, off, 0.62, color=cols)
    for r in b:
        ax[0].annotate("%d" % r.get_height(), xy=(r.get_x() + r.get_width() / 2, r.get_height()),
                       xytext=(0, 4), textcoords="offset points", ha="center",
                       fontsize=10, fontweight="bold")
    ax[0].set_ylabel("MB/s, one core")
    ax[0].set_ylim(0, 6200)
    ax[0].yaxis.set_major_locator(MultipleLocator(1000))
    ax[0].set_title("what the cipher can do\n(no network, cool chip)",
                    fontsize=12, fontweight="bold", loc="left")

    st = {l[1]: stats(l[1]) for l in LIBS}
    mean = [st[l[1]]["mean"] for l in LIBS]
    b = ax[1].bar(x, mean, 0.62, color=cols)
    for i, r in enumerate(b):
        ax[1].annotate("%d" % r.get_height(), xy=(r.get_x() + r.get_width() / 2, r.get_height()),
                       xytext=(0, 4), textcoords="offset points", ha="center",
                       fontsize=10, fontweight="bold")
    ax[1].axhline(1894, color="#666666", lw=1.0, ls="--")
    ax[1].annotate("link ceiling, 1894 MB/s", xy=(3.42, 1930), ha="right",
                   fontsize=8.5, color="#666666")
    ax[1].set_ylabel("MB/s, encrypted and sent")
    ax[1].set_ylim(0, 2300)
    ax[1].yaxis.set_major_locator(MultipleLocator(500))
    ax[1].set_title("what it sustains over the link\n(2 minutes, mean)",
                    fontsize=12, fontweight="bold", loc="left")

    # as a change, not a loss: throughput given away to throttling is negative
    dec = [-st[l[1]]["decay"] for l in LIBS]
    b = ax[2].bar(x, dec, 0.62, color=cols)
    for r, d in zip(b, dec):
        ax[2].annotate("%+.1f%%" % d, xy=(r.get_x() + r.get_width() / 2, r.get_height()),
                       xytext=(0, 5 if d >= 0 else -15), textcoords="offset points",
                       ha="center", fontsize=10, fontweight="bold")
    ax[2].axhline(0, color="#9a9aa2", lw=0.9)
    ax[2].set_ylabel("throughput change over 2 min, %")
    ax[2].set_ylim(-21, 5)
    ax[2].yaxis.set_major_locator(MultipleLocator(5))
    ax[2].set_title("how much it throttles away\n(last 30 s vs first 30 s)",
                    fontsize=12, fontweight="bold", loc="left")

    for a in ax:
        a.set_xticks(list(x))
        a.set_xticklabels(names, fontsize=10)
        a.grid(True, axis="y", color="#e2e2e8", lw=0.8)
        a.set_axisbelow(True)

    fig.suptitle("The same cipher, four libraries: the efficient ones hold the line rate, the slow ones throttle away",
                 x=0.008, ha="left", fontsize=14, fontweight="bold", y=1.045)
    fig.subplots_adjust(wspace=0.30)
    footer(fig, "Encrypting 32 KB packets and pushing them over raw TCP to the Linux box -- as close to "
                "\"ssh built with this cipher\" as possible without building four ssh.exes.\n"
                "Each library ran 2 minutes after cooling the CPU to ~60 C; the order was reversed in a second "
                "pass and the result held (CNG −13.1%, ISA-L +0.7%).", y=-0.06)
    save(fig, "windows-perf-tb-cipher-libs")


# --------------------------------------------------------------- chart 2
def thermal_chart():
    fig, ax = plt.subplots(3, 1, figsize=(11, 9), sharex=True,
                           gridspec_kw={"height_ratios": [3, 2, 2], "hspace": 0.16})

    for name, tag, col in LIBS:
        t = read_thr(tag)
        ax[0].plot([p[0] for p in t], [p[1] for p in t], color=col, lw=1.4, label=name)
        e = read_env(tag)
        ax[1].plot([p[0] for p in e], [p[3] for p in e], color=col, lw=1.4)
        ax[2].plot([p[0] for p in e], [p[1] for p in e], color=col, lw=1.4)

    ax[0].axhline(1894, color="#666666", lw=1.0, ls="--")
    ax[0].annotate("link ceiling, 1894 MB/s", xy=(1.5, 1908), ha="left",
                   fontsize=8.5, color="#666666")
    ax[0].set_ylabel("MB/s encrypted\nand sent")
    ax[0].set_ylim(600, 2050)
    ax[0].yaxis.set_major_locator(MultipleLocator(300))
    ax[0].legend(loc="upper right", frameon=False, fontsize=9.5, ncol=4,
                 bbox_to_anchor=(1.0, 0.99))
    ax[0].annotate("ISA-L and OpenSSL sit on the link ceiling for the whole run;\n"
                   "CNG and LibreSSL start lower and sink as the chip heats",
                   xy=(0.015, 0.10), xycoords="axes fraction", fontsize=9, color="#55555c")

    ax[1].set_ylabel("CPU die\ntemp, °C")
    ax[1].set_ylim(50, 100)
    ax[1].yaxis.set_major_locator(MultipleLocator(10))
    ax[1].annotate("every library ends up hot and the fan pinned at ~6000 rpm in all four runs;\n"
                   "ISA-L ends hottest precisely because it is delivering the most bytes",
                   xy=(0.015, 0.06), xycoords="axes fraction", fontsize=9, color="#55555c")

    ax[2].set_ylabel("effective clock,\n% of base")
    ax[2].set_ylim(120, 200)
    ax[2].yaxis.set_major_locator(MultipleLocator(20))
    ax[2].axhline(100, color="#9a9aa2", lw=0.8, ls="--")
    ax[2].set_xlabel("seconds")
    ax[2].set_xlim(0, 120)
    ax[2].xaxis.set_major_locator(MultipleLocator(10))
    ax[2].annotate("ISA-L holds a higher clock here only because this run started 7 °C cooler.\n"
                   "In the reversed pass it ran at the same 167% as CNG and still sent 1809 MB/s\n"
                   "against CNG's 1618 — fewer cycles per byte is what carries it, not more clock.",
                   xy=(0.015, 0.06), xycoords="axes fraction", fontsize=9, color="#55555c")

    for a in ax:
        a.grid(True, axis="y", color="#e2e2e8", lw=0.8)
        a.set_axisbelow(True)

    fig.suptitle("Two minutes of AES-128-GCM at line rate: throughput, temperature, clock",
                 x=0.055, ha="left", fontsize=14, fontweight="bold", y=0.955)
    footer(fig, "Each run encrypts 32 KB packets and sends them over raw TCP to the Linux box, which logged "
                "the same rates it received (within 1 MB/s).\nThe receiver never exceeded 7% CPU, so nothing "
                "here is the far end.", y=-0.015)
    save(fig, "windows-perf-tb-cipher-thermal")


libs_chart()
thermal_chart()
