# chartdata.ps1 -- fold this session's raw logs into one tidy file per chart,
# so the plotting can be done later without re-reading the run logs.
$sp = $PSScriptRoot
$out = Join-Path $sp 'charts-xperf'
New-Item -ItemType Directory -Force $out | Out-Null

# 1. the three ceilings, interleaved in one thermal state
if (Test-Path "$sp\ceiling.csv") {
    Import-Csv "$sp\ceiling.csv" | Group-Object what | ForEach-Object {
        [pscustomobject]@{
            arm      = $_.Name
            MBps     = [math]::Round(($_.Group | Measure-Object MBps -Average).Average, 0)
            n        = $_.Group.Count
            clockPct = [math]::Round(($_.Group | Measure-Object clockPct -Average).Average, 1)
        }
    } | Export-Csv -NoTypeInformation "$out\ceilings.csv"
}

# 2. parallel streams: where the aggregate stops climbing
if (Test-Path "$sp\parallel.csv") {
    Import-Csv "$sp\parallel.csv" | Group-Object streams | ForEach-Object {
        [pscustomobject]@{
            streams   = [int]$_.Name
            aggMBps   = [math]::Round(($_.Group | Measure-Object aggMBps -Average).Average, 0)
            perStream = [math]::Round(($_.Group | Measure-Object perStream -Average).Average, 0)
        }
    } | Sort-Object streams | Export-Csv -NoTypeInformation "$out\parallel-streams.csv"
}

# 3. fresh vs already-allocated destination on the far side
if (Test-Path "$sp\prealloc.csv") {
    Import-Csv "$sp\prealloc.csv" | Where-Object { [int]$_.MBps -gt 500 } |
        Group-Object arm | ForEach-Object {
            [pscustomobject]@{
                arm  = $_.Name
                MBps = [math]::Round(($_.Group | Measure-Object MBps -Average).Average, 0)
                n    = $_.Group.Count
            }
        } | Export-Csv -NoTypeInformation "$out\remote-prealloc.csv"
}

# 4. every A/B arm: throughput, cost and what the clock was doing.  The spin
#    sweep is the interesting one -- throughput and effective clock fall
#    together, which is the thermal argument in one table.
if (Test-Path "$sp\runs.csv") {
    Import-Csv "$sp\runs.csv" | Where-Object { $_.tag -match '=' -or $_.tag -match '^ring' } |
        Group-Object tag, dir | ForEach-Object {
            $g = $_.Group
            [pscustomobject]@{
                arm       = ($_.Name -split ', ')[0]
                dir       = ($_.Name -split ', ')[1]
                n         = $g.Count
                MBps      = [math]::Round(($g | Measure-Object MBps -Average).Average, 0)
                cpuPerGB  = [math]::Round(($g | Measure-Object cpuPerGB -Average).Average, 3)
                clockPct  = [math]::Round(($g | Measure-Object clockPct -Average).Average, 1)
                tempMaxC  = [math]::Round(($g | Measure-Object tempMax -Average).Average, 0)
            }
        } | Sort-Object dir, arm | Export-Csv -NoTypeInformation "$out\ab-arms.csv"
}

# 5. the CPU profiles, typed in from the butterfly reports so a chart does not
#    have to re-parse xperf HTML
@(
  [pscustomobject]@{ proc='rsync.exe'; dir='push'; item='kernel memcpy (read from cache)'; pct=21.9 }
  [pscustomobject]@{ proc='rsync.exe'; dir='push'; item='memcpy_repmovs (own copies)';     pct=21.4 }
  [pscustomobject]@{ proc='rsync.exe'; dir='push'; item='cache-manager page bookkeeping';  pct=18.0 }
  [pscustomobject]@{ proc='rsync.exe'; dir='push'; item='XXH3_64bits_update';              pct=7.5  }
  [pscustomobject]@{ proc='ssh.exe';   dir='push'; item='AES-GCM (ISA-L)';                 pct=21.0 }
  [pscustomobject]@{ proc='ssh.exe';   dir='push'; item='probe/lock user pages for send';  pct=13.6 }
  [pscustomobject]@{ proc='ssh.exe';   dir='push'; item='memcpy_repmovs';                  pct=8.8  }
  [pscustomobject]@{ proc='ssh.exe';   dir='push'; item='afd.sys memcpy';                  pct=7.1  }
  [pscustomobject]@{ proc='ssh.exe';   dir='push'; item='TCP checksum in software';        pct=4.1  }
) | Export-Csv -NoTypeInformation "$out\cpu-profile.csv"

# 6. per-thread busyness: the "nothing is saturated" chart
@(
  [pscustomobject]@{ dir='push'; thread='ssh.exe busiest';  pctOfCore=64.8 }
  [pscustomobject]@{ dir='push'; thread='rsync.exe main';   pctOfCore=61.0 }
  [pscustomobject]@{ dir='push'; thread='ssh.exe second';   pctOfCore=44.2 }
  [pscustomobject]@{ dir='push'; thread='ssh.exe third';    pctOfCore=36.0 }
  [pscustomobject]@{ dir='pull'; thread='ssh.exe busiest';  pctOfCore=58.9 }
  [pscustomobject]@{ dir='pull'; thread='rsync.exe main';   pctOfCore=49.0 }
  [pscustomobject]@{ dir='pull'; thread='ssh.exe second';   pctOfCore=29.7 }
  [pscustomobject]@{ dir='pull'; thread='ssh.exe third';    pctOfCore=3.6  }
) | Export-Csv -NoTypeInformation "$out\thread-busy.csv"

# 7. the far side, sampled from /proc during each direction
@(
  [pscustomobject]@{ dir='push'; proc='remote rsync (receiver)'; pctOfCoreLow=80; pctOfCoreHigh=100 }
  [pscustomobject]@{ dir='push'; proc='remote sshd-session';     pctOfCoreLow=70; pctOfCoreHigh=90  }
  [pscustomobject]@{ dir='pull'; proc='remote rsync (sender)';   pctOfCoreLow=50; pctOfCoreHigh=80  }
  [pscustomobject]@{ dir='pull'; proc='remote sshd-session';     pctOfCoreLow=50; pctOfCoreHigh=70  }
) | Export-Csv -NoTypeInformation "$out\remote-cpu.csv"

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
