# ceiling.ps1 -- what is actually the limit?  Three arms, interleaved so the
# laptop's thermal drift lands on all of them equally:
#
#   rsync  : rsync push of a cached 4 GB file over the bundled ssh (shm rings)
#   ssh    : the same ssh fed from memory by spew (a pipe, no rsync)
#   iperf3 : one raw TCP stream, no crypto
#
# Logs throughput plus clock, CPU and temperature for each run.
param([int] $Rounds = 3, [int] $Seconds = 4)

$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$rip   = '169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$o     = '-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new'
$R     = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$S     = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = "$sp\perf\big.bin"
$size  = (Get-Item $src).Length

function Get-TempC {
    try {
        $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction Stop |
             Where-Object { $_.ElementName -like '*CPU Probe*' } | Select-Object -First 1
        if ($s) { return [int]$s.CurrentReading }
    } catch {}
    return $null
}
$clk = New-Object Diagnostics.PerformanceCounter('Processor Information','% Processor Performance','_Total')
$null = $clk.NextValue()

function Log-Row($what, $mbps, $secs, $t0, $t1, $clkPct) {
    [pscustomobject]@{ stamp=(Get-Date).ToString('s'); what=$what
        MBps=[math]::Round($mbps,0); elapsedS=[math]::Round($secs,2)
        clockPct=[math]::Round($clkPct,1); tempStart=$t0; tempEnd=$t1 }
}

$rows = @()
for ($i = 0; $i -lt $Rounds; $i++) {

    # --- arm 1: rsync push --------------------------------------------
    & $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"; mkdir -p /tmp/perf" 2>&1 | Out-Null
    $t0 = Get-TempC; $null = $clk.NextValue()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $R -a --inplace -e "$S -c aes128-gcm@openssh.com" $src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    $sw.Stop()
    $rows += Log-Row 'rsync push' ($size/$sw.Elapsed.TotalSeconds/1e6) $sw.Elapsed.TotalSeconds $t0 (Get-TempC) $clk.NextValue()
    & $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null

    # --- arm 2: ssh alone, fed from memory ----------------------------
    $bat = "$sp\spewrun.bat"
    $err = "$sp\spewrun.err"
    @('@echo off', "`"$sp\spew.exe`" $Seconds 256 2>`"$err`" | `"$S`" -c aes128-gcm@openssh.com $rhost `"cat > /dev/null`"") |
        Set-Content -Encoding ASCII $bat
    $t0 = Get-TempC; $null = $clk.NextValue()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $bat | Out-Null
    $sw.Stop()
    # spew reports achieved MB/s per second on stderr; take the mean of the
    # steady-state samples rather than the ramp
    $samples = @()
    foreach ($l in (Get-Content $err -ErrorAction SilentlyContinue)) {
        if ($l -match '^([\d.]+),(\d+)$') { $samples += [int]$matches[2] }
    }
    $ss = if ($samples.Count -ge 3) { $samples[1..($samples.Count-1)] } else { $samples }
    $mb = if ($ss.Count) { ($ss | Measure-Object -Average).Average } else { 0 }
    $rows += Log-Row 'ssh alone (pipe)' $mb $sw.Elapsed.TotalSeconds $t0 (Get-TempC) $clk.NextValue()

    # --- arm 3: raw TCP, one stream -----------------------------------
    $t0 = Get-TempC; $null = $clk.NextValue()
    $srv = Start-Process -FilePath $sys -ArgumentList (@($o) + @($rhost, "iperf3 -s -1 -p 5399")) -PassThru -NoNewWindow -RedirectStandardOutput "$sp\iperf-srv.txt" -RedirectStandardError "$sp\iperf-srv.err"
    Start-Sleep -Milliseconds 700
    $j = & iperf3 -c $rip -p 5399 -t $Seconds -f m -J 2>&1 | Out-String
    try   { $bits = ([regex]::Match($j, '"sum_sent"[\s\S]*?"bits_per_second":\s*([\d.]+)')).Groups[1].Value }
    catch { $bits = 0 }
    $rows += Log-Row 'iperf3 1 stream' ([double]$bits/8/1e6) $Seconds $t0 (Get-TempC) $clk.NextValue()
    try { if (-not $srv.HasExited) { $srv.Kill() } } catch {}
}

$rows | Export-Csv -NoTypeInformation -Append "$sp\ceiling.csv"
$rows | Format-Table -AutoSize
""
foreach ($w in 'rsync push','ssh alone (pipe)','iperf3 1 stream') {
    $g = $rows | Where-Object what -eq $w
    if ($g) { "{0,-18}: {1,6:0} MB/s  (n={2})" -f $w, ($g | Measure-Object MBps -Average).Average, $g.Count }
}
