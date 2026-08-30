# runx.ps1 -- one measured transfer, with every metric the charts will want.
#
#   runx.ps1 -Tag base -Dir push
#   runx.ps1 -Tag seqoff -SetEnv 'RSYNC_WIN32_NO_SEQSCAN=1' -Series
#
# Appends one summary row to runs.csv and, with -Series, a per-run time series
# to series-<tag>-<stamp>.csv.  The remote side is tmpfs: the destination is
# created for the run and deleted right after, every time.
param(
    [string] $Tag = 'base',
    [ValidateSet('push', 'pull')] [string] $Dir = 'push',
    [string] $Src = '',
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe',
    [string] $Cipher = 'aes128-gcm@openssh.com',
    [string[]] $SetEnv = @(),
    [switch] $Series,
    [string] $Note = ''
)

$ErrorActionPreference = 'Stop'
$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$sshOpt = '-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new'
if (-not $Src) { $Src = "$sp\perf\g1.bin" }
$size = (Get-Item $Src).Length

function Remote-Clean {
    & $sys @sshOpt $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null
}
function Get-TempC {
    try {
        $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction Stop |
             Where-Object { $_.ElementName -like '*CPU Probe*' } | Select-Object -First 1
        if ($s) { return [int]$s.CurrentReading }
    } catch {}
    return $null
}

# --- environment for the run --------------------------------------------
$saved = @{}
foreach ($e in $SetEnv) {
    $n, $v = $e.Split('=', 2)
    $saved[$n] = [Environment]::GetEnvironmentVariable($n, 'Process')
    [Environment]::SetEnvironmentVariable($n, $v, 'Process')
}

Remote-Clean
$local = "$sp\pullback"
if ($Dir -eq 'pull') {
    # seed the remote copy first, then time the way back
    & $Rsync -a --inplace -e "$Ssh -c $Cipher" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    New-Item -ItemType Directory -Force $local | Out-Null
    if (Test-Path "$local\g1.bin")  { [IO.File]::Delete("$local\g1.bin") }
    if (Test-Path "$local\big.bin") { [IO.File]::Delete("$local\big.bin") }
} else {
    & $sys @sshOpt $rhost "mkdir -p /tmp/perf" 2>&1 | Out-Null
}

$leaf = (Get-Item $Src).Name
$args = if ($Dir -eq 'push') {
    @('-a','--inplace','-e',"`"$Ssh -c $Cipher`"","`"$Src`"","${rhost}:/tmp/perf/")
} else {
    # forward slash: a trailing backslash before the closing quote escapes it
    @('-a','--inplace','-e',"`"$Ssh -c $Cipher`"","${rhost}:/tmp/perf/$leaf","`"$local/`"")
}

$clk = New-Object Diagnostics.PerformanceCounter('Processor Information','% Processor Performance','_Total')
$cpu = New-Object Diagnostics.PerformanceCounter('Processor Information','% Processor Time','_Total')
$null = $clk.NextValue(); $null = $cpu.NextValue()

$tempStart = Get-TempC
$sw = [Diagnostics.Stopwatch]::StartNew()
$t0 = Get-Date
$p  = Start-Process -FilePath $Rsync -ArgumentList $args -PassThru -NoNewWindow

$rows = @(); $rcpu = 0.0; $scpu = 0.0; $tempMax = $tempStart; $lastTemp = $tempStart
$clkAcc = 0.0; $cpuAcc = 0.0; $n = 0; $tick = 0
while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 250
    $tick++
    try {
        $r = Get-Process -Id $p.Id -ErrorAction Stop
        if ($r.TotalProcessorTime.TotalSeconds -gt $rcpu) { $rcpu = $r.TotalProcessorTime.TotalSeconds }
    } catch {}
    foreach ($s in (Get-Process ssh -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt $t0 })) {
        if ($s.TotalProcessorTime.TotalSeconds -gt $scpu) { $scpu = $s.TotalProcessorTime.TotalSeconds }
    }
    $c1 = $clk.NextValue(); $c2 = $cpu.NextValue()
    $clkAcc += $c1; $cpuAcc += $c2; $n++
    if ($tick % 4 -eq 0) {
        $t = Get-TempC
        if ($t) { $lastTemp = $t; if ($t -gt $tempMax) { $tempMax = $t } }
    }
    if ($Series) {
        $rows += [pscustomobject]@{
            t = [math]::Round($sw.Elapsed.TotalSeconds,2)
            rsyncCPUs = [math]::Round($rcpu,3); sshCPUs = [math]::Round($scpu,3)
            clockPct = [math]::Round($c1,1); cpuPct = [math]::Round($c2,1); tempC = $lastTemp
        }
    }
}
$sw.Stop()
$el = $sw.Elapsed.TotalSeconds
try {
    $r = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($r -and $r.TotalProcessorTime.TotalSeconds -gt $rcpu) { $rcpu = $r.TotalProcessorTime.TotalSeconds }
} catch {}
$tempEnd = Get-TempC
$rc = $p.ExitCode

if ($Dir -eq 'pull' -and (Test-Path "$local\$leaf")) { [IO.File]::Delete("$local\$leaf") }
Remote-Clean

foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }

$mbps = $size / $el / 1e6
$row = [pscustomobject]@{
    stamp     = (Get-Date).ToString('s')
    tag       = $Tag
    dir       = $Dir
    srcMB     = [math]::Round($size/1e6,0)
    MBps      = [math]::Round($mbps,0)
    elapsedS  = [math]::Round($el,3)
    rsyncCPUs = [math]::Round($rcpu,3)
    sshCPUs   = [math]::Round($scpu,3)
    cpuPerGB  = [math]::Round(($rcpu+$scpu)/($size/1e9),3)
    clockPct  = if ($n) { [math]::Round($clkAcc/$n,1) } else { $null }
    cpuPct    = if ($n) { [math]::Round($cpuAcc/$n,1) } else { $null }
    tempStart = $tempStart
    tempEnd   = $tempEnd
    tempMax   = $tempMax
    rc        = $rc
    rsyncBuilt= (Get-Item $Rsync).LastWriteTime.ToString('s')
    note      = $Note
}
$row | Export-Csv -NoTypeInformation -Append "$sp\runs.csv"
if ($Series -and $rows.Count) {
    $rows | Export-Csv -NoTypeInformation "$sp\series-$Tag-$((Get-Date).ToString('HHmmss')).csv"
}
"{0,-12} {1,4} {2,6:0} MB/s  {3,6:0.00}s  rsync {4,5:0.00}s  ssh {5,5:0.00}s  {6,6:0.000} CPU-s/GB  clk {7,5:0.0}%  cpu {8,4:0.0}%  temp {9}->{10}C (max {11})  rc={12}" -f `
    $Tag, $Dir, $row.MBps, $row.elapsedS, $row.rsyncCPUs, $row.sshCPUs, $row.cpuPerGB, $row.clockPct, $row.cpuPct, $row.tempStart, $row.tempEnd, $row.tempMax, $rc
