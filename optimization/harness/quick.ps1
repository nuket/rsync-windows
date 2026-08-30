# One push per call, printed as it lands and appended to a log, so a lost
# tool result costs nothing.  1 GB: these transfers reach steady state in
# well under a second, so a longer file only buys thermal drift.
#   quick.ps1 <tag> <envName> <envValue|"">
param([string]$Tag, [string]$EnvName = '', [string]$EnvValue = '')

$rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe'
$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$src   = "$PSScriptRoot\perf\g1.bin"
$host_ = 'claude@169.254.238.153'
$size  = (Get-Item $src).Length

& $ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
if ($EnvName) {
    if ($EnvValue -eq '') { Remove-Item "env:$EnvName" -ErrorAction SilentlyContinue }
    else { Set-Item "env:$EnvName" $EnvValue }
}
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
    Start-Sleep -Milliseconds 25
}
$el = (Get-Date) - $t0
if ($EnvName) { Remove-Item "env:$EnvName" -ErrorAction SilentlyContinue }

$line = "{0,-10} {1,6:0} MB/s   rsync {2,5:0.00}s  ssh {3,5:0.00}s   {4,6:0.000} CPU-s/GB" -f `
    $Tag, ($size / $el.TotalSeconds / 1e6), $rcpu, $scpu, (($rcpu + $scpu) / ($size / 1e9))
$line
Add-Content -Path "$PSScriptRoot\quick.log" -Value $line
