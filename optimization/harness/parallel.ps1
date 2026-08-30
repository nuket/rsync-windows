# parallel.ps1 -- is the far-end receiver the limit?
#
# One push, then N pushes at once to separate destinations.  If the Windows
# sender were the constraint, the aggregate would not move.  If a
# single-threaded receiver on a 16-core box is the constraint, the aggregate
# should climb until the link or the sender binds.
param([int] $N = 2, [int] $Rounds = 2)

$ErrorActionPreference = 'Stop'
$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$o     = @('-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new')
$rsExe = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$shExe = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = Join-Path $sp 'perf\big.bin'
$size  = (Get-Item $src).Length
foreach ($p in $rsExe, $shExe, $src) { if (-not (Test-Path $p)) { throw "missing: $p" } }

function Wipe {
    & $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null
}

function Run-N([int] $n) {
    Wipe
    $dirs = 0..($n-1) | ForEach-Object { "/tmp/perf/d$_" }
    & $sys @o $rhost ("mkdir -p " + ($dirs -join ' ')) 2>&1 | Out-Null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $procs = @()
    for ($i = 0; $i -lt $n; $i++) {
        $a = @('-a','--inplace','-e',"`"$shExe -c aes128-gcm@openssh.com`"","`"$src`"","${rhost}:/tmp/perf/d$i/")
        $procs += Start-Process -FilePath $rsExe -ArgumentList $a -PassThru -NoNewWindow
    }
    $rc = 0
    foreach ($proc in $procs) { $proc.WaitForExit(); if ($proc.ExitCode -ne 0) { $rc = $proc.ExitCode } }
    $sw.Stop()
    Wipe
    $agg = $n * $size / $sw.Elapsed.TotalSeconds / 1e6
    [pscustomobject]@{
        streams   = $n
        aggMBps   = [math]::Round($agg, 0)
        perStream = [math]::Round($agg / $n, 0)
        elapsedS  = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        rc        = $rc
    }
}

$rows = @()
for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($n in 1..$N) { $rows += Run-N $n }
}
$rows | Export-Csv -NoTypeInformation -Append (Join-Path $sp 'parallel.csv')
$rows | Format-Table -AutoSize
''
foreach ($n in 1..$N) {
    $g = $rows | Where-Object { $_.streams -eq $n }
    '{0} stream(s): aggregate {1,6:0} MB/s   per stream {2,6:0} MB/s' -f $n,
        ($g | Measure-Object aggMBps -Average).Average,
        ($g | Measure-Object perStream -Average).Average
}
