# Does the ring size matter?  Two 4 MB rings are 8 MB against a 6 MB L3, so
# the copy in and the copy out may be running at memory speed when they could
# be running at cache speed.  Sizes interleaved, not one after the other, so
# the laptop's thermal drift lands on all of them.
param([int[]] $SizesKB = @(256, 512, 1024, 4096), [int]$Rounds = 3)

$rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = 'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\perf\big.bin'
$host_ = 'claude@169.254.238.153'
$size  = (Get-Item $src).Length

function Run-One($kb) {
    & $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
    $env:RSYNC_WIN32_SHMPIPE_KB = "$kb"
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
    $env:RSYNC_WIN32_SHMPIPE_KB = $null
    [pscustomobject]@{
        ringKB   = $kb
        MBps     = [math]::Round($size / $el.TotalSeconds / 1e6, 0)
        rsyncCPU = [math]::Round($rcpu, 2)
        sshCPU   = [math]::Round($scpu, 2)
        cpuPerGB = [math]::Round(($rcpu + $scpu) / ($size / 1e9), 3)
    }
}

$out = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    foreach ($kb in $SizesKB) { $out += Run-One $kb }
}
& $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null

$out | Sort-Object ringKB | Format-Table -AutoSize
""
"ring KB   MB/s    CPU-s per GB   (means of $Rounds)"
foreach ($kb in $SizesKB) {
    $g = $out | Where-Object ringKB -eq $kb
    "{0,7}  {1,6:0}   {2,8:0.000}" -f $kb,
        ($g | Measure-Object MBps -Average).Average,
        ($g | Measure-Object cpuPerGB -Average).Average
}
