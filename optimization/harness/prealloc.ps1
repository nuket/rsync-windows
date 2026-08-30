# prealloc.ps1 -- is the remote receiver's cost the transfer, or allocating
# 4 GB of tmpfs for a brand-new file every run?
#
#   fresh : destination deleted first, so the far side allocates as it writes
#   reuse : destination already there at full size, overwritten with --inplace
#
# Interleaved, so the laptop's drift lands on both arms.
param([int] $Rounds = 3)

$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$o     = @('-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new')
$rsExe = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$shExe = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = Join-Path $sp 'perf\big.bin'
$size  = (Get-Item $src).Length

function Wipe { & $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null }

function Run-One([string] $arm) {
    Wipe
    & $sys @o $rhost "mkdir -p /tmp/perf" 2>&1 | Out-Null
    if ($arm -eq 'reuse') {
        # real pages, not a sparse hole: the point is that nothing new is
        # allocated while the transfer runs
        & $sys @o $rhost "dd if=/dev/zero of=/tmp/perf/big.bin bs=1M count=4096 status=none; touch -d '2020-01-01' /tmp/perf/big.bin" 2>&1 | Out-Null
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $rsExe -a --inplace --whole-file -e "$shExe -c aes128-gcm@openssh.com" $src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    $sw.Stop()
    $ok = & $sys @o $rhost "stat -c %s /tmp/perf/big.bin" 2>&1
    Wipe
    [pscustomobject]@{ arm = $arm
        MBps = [math]::Round($size/$sw.Elapsed.TotalSeconds/1e6, 0)
        elapsedS = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        remoteBytes = "$ok".Trim() }
}

$rows = @()
for ($r = 0; $r -lt $Rounds; $r++) {
    $rows += Run-One 'fresh'
    $rows += Run-One 'reuse'
}
$rows | Export-Csv -NoTypeInformation -Append (Join-Path $sp 'prealloc.csv')
$rows | Format-Table -AutoSize
''
foreach ($a in 'fresh','reuse') {
    $g = $rows | Where-Object { $_.arm -eq $a }
    '{0,-6}: {1,6:0} MB/s (n={2})' -f $a, ($g | Measure-Object MBps -Average).Average, $g.Count
}

