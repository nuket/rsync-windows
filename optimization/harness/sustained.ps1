# Sustained-transfer test: does a cheaper cipher throttle less and therefore
# hold throughput up over minutes?  Sends the same file over and over for
# -Seconds, sampling effective clock and CPU die temperature throughout, and
# reports the decay from the first transfers to the last.
param(
    [string] $Cipher = 'aes128-gcm@openssh.com',
    [int]    $Seconds = 120,
    [string] $Ssh = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe',
    [string] $File = 'C:\Users\Claude\rsync-perf\s0.bin',
    [string] $Host_ = 'claude@169.254.238.153',
    [string] $Tag = ''          # when set, dump the time series to <Tag>-samples.csv / -runs.csv
)

$mb = (Get-Item $File).Length / 1MB

$sampler = Start-Job -ArgumentList $Seconds -ScriptBlock {
    param($secs)
    $rows = @()
    $t0 = Get-Date
    $end = $t0.AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        $clk = (Get-Counter '\Processor Information(_Total)\% Processor Performance' -MaxSamples 1
               ).CounterSamples[0].CookedValue
        $t = 0
        try {
            $t = (Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor |
                  Where-Object { $_.SensorType -eq 2 -and $_.ElementName -like '*CPU*' } |
                  Select-Object -First 1).CurrentReading
        } catch { }
        $rows += [pscustomobject]@{
            t     = [math]::Round(((Get-Date) - $t0).TotalSeconds, 2)
            clock = [math]::Round($clk, 1)
            temp  = [double]$t
        }
        Start-Sleep -Milliseconds 700
    }
    $rows
}

$runs = @()
$runAt = @()
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $at = $sw.Elapsed.TotalSeconds
    $one = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Ssh -ArgumentList "-c $Cipher $Host_ `"cat > /dev/null`"" `
                       -RedirectStandardInput $File -PassThru -NoNewWindow
    $p.WaitForExit()
    $one.Stop()
    $runs += ($mb / $one.Elapsed.TotalSeconds)
    $runAt += $at
}
$sw.Stop()

$s = Receive-Job $sampler -Wait
Remove-Job $sampler

$n = $runs.Count
$firstK = [math]::Max(1, [int]($n / 4))
$first = ($runs | Select-Object -First $firstK | Measure-Object -Average).Average
$last = ($runs | Select-Object -Last $firstK | Measure-Object -Average).Average
$clks = $s | ForEach-Object { $_.clock }
$temps = $s | Where-Object { $_.temp -gt 0 } | ForEach-Object { $_.temp }

if ($Tag) {
    $s | Export-Csv "$Tag-samples.csv" -NoTypeInformation
    0..($runs.Count - 1) | ForEach-Object {
        [pscustomobject]@{ t = [math]::Round($runAt[$_], 2); mbps = [math]::Round($runs[$_], 1) }
    } | Export-Csv "$Tag-runs.csv" -NoTypeInformation
}

[pscustomobject]@{
    Cipher     = ($Cipher -replace '@openssh.com', '')
    Transfers  = $n
    MeanMBps   = [math]::Round(($runs | Measure-Object -Average).Average, 0)
    FirstMBps  = [math]::Round($first, 0)
    LastMBps   = [math]::Round($last, 0)
    DecayPct   = [math]::Round(100 * (1 - $last / $first), 1)
    ClockStart = [math]::Round(($clks | Select-Object -First 5 | Measure-Object -Average).Average, 0)
    ClockEnd   = [math]::Round(($clks | Select-Object -Last 5 | Measure-Object -Average).Average, 0)
    ClockMean  = [math]::Round(($clks | Measure-Object -Average).Average, 0)
    TempStart  = if ($temps) { $temps[0] } else { 0 }
    TempEnd    = if ($temps) { $temps[-1] } else { 0 }
    TempMax    = if ($temps) { ($temps | Measure-Object -Maximum).Maximum } else { 0 }
}
