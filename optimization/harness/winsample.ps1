# Sample Windows CPU, effective clock, Thunderbolt NIC throughput and the
# Dell Command | Monitor temperature probes once a second.
#
# CSV: t,cpu_pct,perf_pct,mhz,tx_MBps,rx_MBps,cpu_c,ambient_c
#
# Numbers are written with custom formats ("0.00"), never "N2" -- the latter
# inserts a thousands separator and quietly breaks the CSV.
param([int]$Samples = 185, [string]$Out = "win-samples.csv")

$counters = @(
    '\Processor Information(_Total)\% Processor Time',
    '\Processor Information(_Total)\% Processor Performance',
    '\Processor Information(_Total)\Processor Frequency',
    '\Network Interface(Thunderbolt[TM] Networking)\Bytes Sent/sec',
    '\Network Interface(Thunderbolt[TM] Networking)\Bytes Received/sec'
)

function Get-Temps {
    # Dell reports UnitModifier -1 but CurrentReading is already whole
    # degrees C on this machine (56 idle, 91 under load).
    try {
        $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction Stop |
             Where-Object { $_.SensorType -eq 2 }
        $cpu = ($s | Where-Object { $_.ElementName -like '*CPU*' } | Select-Object -First 1).CurrentReading
        $amb = ($s | Where-Object { $_.ElementName -like '*Ambient*' } | Select-Object -First 1).CurrentReading
        return @([double]$cpu, [double]$amb)
    } catch {
        return @(0.0, 0.0)
    }
}

"t,cpu_pct,perf_pct,mhz,tx_MBps,rx_MBps,cpu_c,ambient_c" | Set-Content $Out
$t0 = Get-Date
Get-Counter -Counter $counters -SampleInterval 1 -MaxSamples $Samples | ForEach-Object {
    $v = @{}
    foreach ($s in $_.CounterSamples) { $v[$s.Path] = $s.CookedValue }
    $get = {
        param($frag)
        foreach ($k in $v.Keys) { if ($k -like "*$frag*") { return $v[$k] } }
        return 0
    }
    $t = Get-Temps
    $row = ('{0:0.00},{1:0.00},{2:0.00},{3:0},{4:0.0},{5:0.0},{6:0},{7:0}' -f `
        ((Get-Date) - $t0).TotalSeconds,
        (& $get '% processor time'),
        (& $get '% processor performance'),
        (& $get 'processor frequency'),
        ((& $get 'bytes sent/sec') / 1e6),
        ((& $get 'bytes received/sec') / 1e6),
        $t[0], $t[1])
    Add-Content -Path $Out -Value $row
}
