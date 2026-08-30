# What the kernel's send buffer is worth: SSH_SOCK_SNDBUF values interleaved
# so the laptop's thermal drift lands on all of them.  "" is Windows' own
# autotuning, 0 is no buffering at all (the send goes straight from our
# block to the wire).
param([string[]] $Bufs = @('', '0'), [int]$Rounds = 4)

$rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = 'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\perf\big.bin'
$host_ = 'claude@169.254.238.153'
$size  = (Get-Item $src).Length

function Run-One($buf) {
    & $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
    if ($buf -eq '') { $env:SSH_SOCK_SNDBUF = $null } else { $env:SSH_SOCK_SNDBUF = $buf }
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
    $env:SSH_SOCK_SNDBUF = $null
    [pscustomobject]@{
        sndbuf   = if ($buf -eq '') { 'auto' } else { $buf }
        MBps     = [math]::Round($size / $el.TotalSeconds / 1e6, 0)
        rsyncCPU = [math]::Round($rcpu, 2)
        sshCPU   = [math]::Round($scpu, 2)
        cpuPerGB = [math]::Round(($rcpu + $scpu) / ($size / 1e9), 3)
    }
}

$out = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    foreach ($b in $Bufs) { $out += Run-One $b }
}
& $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null

$out | Export-Csv -NoTypeInformation -Path "$PSScriptRoot\sndbuf-runs.csv"
$out | Format-Table -AutoSize
""
"SO_SNDBUF     MB/s   ssh CPU-s   CPU-s per GB   (means of $Rounds)"
foreach ($b in $Bufs) {
    $tag = if ($b -eq '') { 'auto' } else { $b }
    $g = $out | Where-Object sndbuf -eq $tag
    "{0,9}  {1,6:0}   {2,9:0.00}   {3,8:0.000}" -f $tag,
        ($g | Measure-Object MBps -Average).Average,
        ($g | Measure-Object sshCPU -Average).Average,
        ($g | Measure-Object cpuPerGB -Average).Average
}
