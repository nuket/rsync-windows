# Same push, both transports, watching what each costs in CPU as well as in
# wall time: the ring's case is that it saves work, which on a 15W laptop is
# not the same question as whether it saves seconds.
param([int]$Rounds = 3)

$rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = 'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\perf\big.bin'
$host_ = 'claude@169.254.238.153'
$size  = (Get-Item $src).Length

function Run-One($ring, $tag) {
    & $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
    if ($ring) { $env:RSYNC_WIN32_NO_SHMPIPE = $null } else { $env:RSYNC_WIN32_NO_SHMPIPE = '1' }

    $t0 = Get-Date
    $p = Start-Process -FilePath $rsync -ArgumentList @(
        '-a', '--inplace', '-e', "`"$ssh -c aes128-gcm@openssh.com`"", "`"$src`"", "${host_}:/tmp/perf/"
    ) -PassThru -NoNewWindow
    $rcpu = 0.0; $scpu = 0.0
    while (-not $p.HasExited) {
        try {
            $r = Get-Process -Id $p.Id -ErrorAction Stop
            if ($r.TotalProcessorTime.TotalSeconds -gt $rcpu) { $rcpu = $r.TotalProcessorTime.TotalSeconds }
        } catch {}
        foreach ($s in (Get-Process ssh -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt $t0 })) {
            if ($s.TotalProcessorTime.TotalSeconds -gt $scpu) { $scpu = $s.TotalProcessorTime.TotalSeconds }
        }
        Start-Sleep -Milliseconds 50
    }
    $el = (Get-Date) - $t0
    $env:RSYNC_WIN32_NO_SHMPIPE = $null
    $gb = $size / 1e9
    [pscustomobject]@{
        transport = $tag
        MBps      = [math]::Round($size / $el.TotalSeconds / 1e6, 0)
        rsyncCPU  = [math]::Round($rcpu, 2)
        sshCPU    = [math]::Round($scpu, 2)
        cpuPerGB  = [math]::Round(($rcpu + $scpu) / $gb, 3)
    }
}

$out = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    $out += Run-One $false 'pipe'
    $out += Run-One $true  'shm'
}
& $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
$out | Format-Table -AutoSize
foreach ($t in 'pipe', 'shm') {
    $g = $out | Where-Object transport -eq $t
    "{0}: {1:0} MB/s, rsync {2:0.00}s CPU, ssh {3:0.00}s CPU, {4:0.000} CPU-s per GB" -f $t,
        ($g | Measure-Object MBps -Average).Average,
        ($g | Measure-Object rsyncCPU -Average).Average,
        ($g | Measure-Object sshCPU -Average).Average,
        ($g | Measure-Object cpuPerGB -Average).Average
}
