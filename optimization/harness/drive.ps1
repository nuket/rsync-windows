# Run one AES-128-GCM library flat out for -Seconds, encrypting 32 KB packets
# and pushing them over raw TCP to the Linux box -- as close to "ssh built
# with this cipher" as we can get without building four ssh.exes.
#
# Logs, once a second: the sender's payload throughput, the receiver's, and
# on both machines the CPU load and die temperature (plus fan speed and
# effective clock on Windows).  Cools the chip to a common temperature first
# so four libraries run back to back can be compared fairly.
param(
    [Parameter(Mandatory)] [string] $Backend,
    [int]    $Seconds = 120,
    [Parameter(Mandatory)] [string] $Out,
    [int]    $CoolTo = 60,
    [int]    $CoolMax = 100,
    [string] $Target = '169.254.238.153',
    [int]    $Port = 5301,
    [string] $RemoteUser = 'claude@169.254.238.153',
    [string] $Ssh = 'C:\Windows\System32\OpenSSH\ssh.exe',
    [string] $Exe = 'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\gcmsustain.exe'
)

function Get-Dell {
    $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction SilentlyContinue
    $t = ($s | Where-Object { $_.SensorType -eq 2 -and $_.ElementName -like '*CPU*' } | Select-Object -First 1).CurrentReading
    $f = ($s | Where-Object { $_.SensorType -eq 5 } | Select-Object -First 1).CurrentReading
    return @([double]$t, [double]$f)
}

# ---- cool to a common starting point
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $CoolMax) {
    $d = Get-Dell
    if ($d[0] -le $CoolTo -and $d[0] -gt 0) { break }
    Start-Sleep -Seconds 3
}
$d0 = Get-Dell
Write-Host ("   {0,-9} start {1} C, fan {2} rpm (cooled {3:N0} s)" -f `
            $Backend, $d0[0], $d0[1], $sw.Elapsed.TotalSeconds)

# ---- Linux sink, logging its own side
$remoteCsv = "/tmp/tb/sink-$Backend.csv"
Start-Process -FilePath $Ssh -ArgumentList `
    "$RemoteUser `"python3 /tmp/tb/linsink.py $Port $($Seconds + 20) thunderbolt0 $remoteCsv`"" `
    -PassThru -NoNewWindow -RedirectStandardOutput "$Out-sink.log" | Out-Null
Start-Sleep -Seconds 2

# ---- run and sample
$thr = "$Out-thr.csv"
$p = Start-Process -FilePath $Exe -ArgumentList "$Backend $Seconds $Target $Port" `
                   -PassThru -NoNewWindow -RedirectStandardOutput $thr
$rows = @()
$t0 = Get-Date
while (-not $p.HasExited) {
    $clk = (Get-Counter '\Processor Information(_Total)\% Processor Performance' -MaxSamples 1).CounterSamples[0].CookedValue
    $cpu = (Get-Counter '\Processor Information(_Total)\% Processor Time' -MaxSamples 1).CounterSamples[0].CookedValue
    $d = Get-Dell
    $rows += [pscustomobject]@{
        t     = [math]::Round(((Get-Date) - $t0).TotalSeconds, 2)
        clock = [math]::Round($clk, 1)
        cpu   = [math]::Round($cpu, 1)
        temp  = $d[0]
        fan   = $d[1]
    }
    Start-Sleep -Milliseconds 400
}
$rows | Export-Csv "$Out-env.csv" -NoTypeInformation

Start-Sleep -Seconds 3
cmd /c "`"$Ssh`" $RemoteUser `"cat $remoteCsv`" > `"$Out-lin.csv`"" | Out-Null

# ---- summary
$t = @()
foreach ($line in Get-Content $thr) {
    $f = $line.Split(',')
    if ($f.Count -eq 2) { $t += [double]$f[1] }
}
if (-not $t) { $t = @(0) }
$n = [math]::Max(1, [int]($t.Count / 4))
$first = ($t | Select-Object -First $n | Measure-Object -Average).Average
$last = ($t | Select-Object -Last $n | Measure-Object -Average).Average
[pscustomobject]@{
    Backend   = $Backend
    MeanMBps  = [math]::Round(($t | Measure-Object -Average).Average, 0)
    FirstMBps = [math]::Round($first, 0)
    LastMBps  = [math]::Round($last, 0)
    DecayPct  = [math]::Round(100 * (1 - $last / [math]::Max($first, 1)), 1)
    MinMBps   = [math]::Round(($t | Measure-Object -Minimum).Minimum, 0)
    MaxMBps   = [math]::Round(($t | Measure-Object -Maximum).Maximum, 0)
    CpuMean   = [math]::Round(($rows | Measure-Object cpu -Average).Average, 1)
    ClockMean = [math]::Round(($rows | Measure-Object clock -Average).Average, 0)
    ClockEnd  = [math]::Round(($rows | Select-Object -Last 5 | Measure-Object clock -Average).Average, 0)
    TempStart = $d0[0]
    TempMax   = ($rows | Measure-Object temp -Maximum).Maximum
    FanMax    = ($rows | Measure-Object fan -Maximum).Maximum
}
