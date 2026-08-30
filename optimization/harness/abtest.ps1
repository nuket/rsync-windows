# Push the same 4 GB file over the Thunderbolt link, alternating the two
# transports, so the machine's thermal drift lands on both equally.
param([int]$Rounds = 3)

$rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$src   = 'C:\Users\Claude\AppData\Local\Temp\claude\C--Users-Claude-devsrc-rsync-windows\66c00488-88a8-48c3-be31-6abc940b8590\scratchpad\perf\big.bin'
$host_ = 'claude@169.254.238.153'
$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$size  = (Get-Item $src).Length

function Run-One($ring, $tag) {
    & $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
    if ($ring) { $env:RSYNC_WIN32_NO_SHMPIPE = $null } else { $env:RSYNC_WIN32_NO_SHMPIPE = '1' }
    $t0 = Get-Date
    & $rsync -a --inplace -e "$ssh -c aes128-gcm@openssh.com" $src "${host_}:/tmp/perf/" 2>&1 | Out-Null
    $rc = $LASTEXITCODE
    $el = (Get-Date) - $t0
    $env:RSYNC_WIN32_NO_SHMPIPE = $null
    $mbs = [math]::Round($size / $el.TotalSeconds / 1e6, 0)
    [pscustomobject]@{ transport = $tag; MBps = $mbs; seconds = [math]::Round($el.TotalSeconds, 2); rc = $rc }
}

$out = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    $out += Run-One $false 'pipe'
    $out += Run-One $true  'shm'
}
& $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
$out | Format-Table -AutoSize
"pipe mean: {0:0} MB/s   shm mean: {1:0} MB/s" -f `
    (($out | Where-Object transport -eq 'pipe' | Measure-Object MBps -Average).Average), `
    (($out | Where-Object transport -eq 'shm'  | Measure-Object MBps -Average).Average)
