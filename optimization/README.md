# Optimisation measurements

Everything behind the `windows-perf-*` charts in the repository root: the run
logs, the scripts that produced them, the standalone microbenchmarks, and the
raw profiler output. Recovered from two sessions' scratch directories, which
are temporary and would otherwise have been cleaned.

    optimization/
      charts/     one generator per round of work; each writes SVG + PNG
      harness/    the measurement scripts -- what actually ran the transfers
      bench/      standalone microbenchmarks, built and run on their own
      data/       every measured CSV/JSON
      profiles/   xperf butterfly reports, sampler output, per-process CPU logs

## Rebuilding the charts

Needs `matplotlib`. From the repository root:

    python optimization/charts/perfcharts.py   .                      # 2.5 GbE era
    python optimization/charts/tbcharts.py     .                      # Thunderbolt round + power plan
    python optimization/charts/shmcharts.py    .                      # shared-memory ring
    python optimization/charts/isalcharts.py   .                      # ISA-L, sustained/thermal
    python optimization/charts/ciphercharts.py optimization/data .    # cipher libraries
    python optimization/charts/linkchart.py    optimization/data/iperf-run2 windows-perf-thunderbolt-link-3min
    python optimization/charts/xperfcharts.py  .                      # xperf round + the overall arc

All seven were re-run from this directory to confirm the salvage is complete;
between them they reproduce every published chart.

Generators differ in where their figures live, because they were written at
different times: `perfcharts`, `tbcharts` and `shmcharts` carry their numbers
inline (taken from commit messages and the porting log), while `ciphercharts`,
`isalcharts`, `linkchart` and `xperfcharts` read the CSVs in `data/`.

## How to read the numbers

Every transfer is 4 GB of a page-cache-warm file over `aes128-gcm`, Windows ↔
Linux across Thunderbolt networking, with the destination created for the run
and deleted straight after (the far side's `/tmp` is a RAM disk). This laptop
is a 15 W part and throttles hard: `% Processor Performance` reads ~185% of
base cold and decays past 135% when hot. **Arms of a comparison are therefore
always interleaved within one session**, and in the later rounds cooled to a
fixed temperature between rounds. Numbers from different sessions are not
comparable, and the charts say so where it matters.

`data/perflog.csv` is the running log of every headline figure across all the
sessions — the single most useful file here if you want one number for
something.

## Notable data sets

| file(s) | what they measured |
| --- | --- |
| `perflog.csv` | every headline figure, all rounds |
| `runs.csv` | the xperf round: per-run MB/s, rsync/ssh CPU, CPU-s/GB, clock %, temperatures |
| `ceiling.csv`, `parallel.csv`, `prealloc.csv` | the three tests that located the bottleneck at the far end |
| `thermal-*.csv`, `net-*.csv`, `rev-*.csv` | cipher libraries held at line rate for minutes, with clock and temperature |
| `a128-*.csv`, `a256-*.csv` | aes128 vs aes256 end to end |
| `ab-*.csv` | single-knob A/B sweeps (mapped reads, read window, iobuf size, sequential hint, GCM backend) |
| `iperf-run*/` | the 3-minute link characterisation, both ends sampled |
| `profiles/*-stack-*.txt` | xperf butterfly reports (CPU samples and context-switch stacks, rsync.exe and ssh.exe, push and pull) |
| `profiles/cpu-*.log`, `profiles/trace-*.log` | earlier per-process CPU and ssh syscall traces |

## Rows deliberately removed

Two batches of runs were mistakes rather than measurements, and were deleted
from the raw logs so nobody averages them in:

* Six rows in `parallel.csv` from a run whose `Start-Process` could not find
  `rsync.exe`, so it timed an empty loop and reported 5,680–129,444 MB/s.
* Six rows in `prealloc.csv` from a round that omitted `--whole-file`, so an
  existing destination sent rsync down the delta path — block checksums over
  4 GB against all-zero content — and the arm read 91–97 MB/s. That measures
  the mistake, not tmpfs.

## Redactions

Two edits were made to the salvaged files, neither touching a measurement:

* `profiles/trace-stock-file.log` and `profiles/trace-pump-file.log` are
  `ssh -vvv` traces, and their connection setup listed key file paths,
  `known_hosts` and `authorized_keys` entries, home directories and four
  SHA256 public-key fingerprints. Sixteen lines were removed from each. No
  private key material was ever in them. What the files are kept for — the
  syscall and channel sequence of the data path — is untouched, and each
  carries a header saying so.
* The Windows machine's hostname in the `system_info` field of the three
  `iperf3*.json` files was replaced with `WINHOST`.

## Experiments that were measured and then reverted

The `SSH_SHMIO_SPIN` and `RSYNC_WIN32_WSEQSCAN` arms in `runs.csv`, and the
mapped-reader and read-window arms in `ab-*.csv`, are all changes that were
built, measured, and taken back out. None is in the tree. WINDOWS-PORT.md has
the reasoning, under "Moar Speed Pt. 9!" and "Moar Speed Pt. X!".
