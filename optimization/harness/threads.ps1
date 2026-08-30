# threads.ps1 -- per-thread CPU for rsync.exe and ssh.exe across one push.
# If a single thread sits at ~100% of a core, that thread is the ceiling.
param(
    [ValidateSet('push','pull')] [string] $Dir = 'push',
    [string] $Src = '',
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe',
    [string] $Cipher = 'aes128-gcm@openssh.com'
)
$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$o     = '-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new'
if (-not $Src) { $Src = "$sp\perf\big.bin" }
$size = (Get-Item $Src).Length

& $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null
& $sys @o $rhost "mkdir -p /tmp/perf" 2>&1 | Out-Null

$leaf  = (Get-Item $Src).Name
$local = "$sp\pullback"
if ($Dir -eq 'pull') {
    & $Rsync -a --inplace -e "$Ssh -c $Cipher" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    New-Item -ItemType Directory -Force $local | Out-Null
    if (Test-Path "$local\$leaf") { [IO.File]::Delete("$local\$leaf") }
}
$rargs = if ($Dir -eq 'push') {
    @('-a','--inplace','-e',"`"$Ssh -c $Cipher`"","`"$Src`"","${rhost}:/tmp/perf/")
} else {
    @('-a','--inplace','-e',"`"$Ssh -c $Cipher`"","${rhost}:/tmp/perf/$leaf","`"$local/`"")
}

$t0 = Get-Date
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $Rsync -ArgumentList $rargs -PassThru -NoNewWindow

# thread id -> last seen cumulative CPU seconds
$seen = @{}
while (-not $p.HasExited) {
    foreach ($proc in @(Get-Process -Id $p.Id -ErrorAction SilentlyContinue) +
                      @(Get-Process ssh -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt $t0 })) {
        foreach ($t in $proc.Threads) {
            try {
                $key = "$($proc.ProcessName)/$($t.Id)"
                $cpu = $t.TotalProcessorTime.TotalSeconds
                if (-not $seen.ContainsKey($key) -or $cpu -gt $seen[$key]) { $seen[$key] = $cpu }
            } catch {}
        }
    }
    Start-Sleep -Milliseconds 40
}
$sw.Stop()
$el = $sw.Elapsed.TotalSeconds
if ($Dir -eq 'pull' -and (Test-Path "$local\$leaf")) { [IO.File]::Delete("$local\$leaf") }
& $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null

"{0} {1:0} MB/s over {2:0.00}s" -f $Dir, ($size/$el/1e6), $el
""
"{0,-22} {1,8} {2,10}" -f 'process/thread','CPU-s','% of core'
$tot = 0.0
foreach ($k in ($seen.Keys | Sort-Object { -$seen[$_] })) {
    if ($seen[$k] -lt 0.02) { continue }
    $tot += $seen[$k]
    "{0,-22} {1,8:0.000} {2,9:0.0}%" -f $k, $seen[$k], (100 * $seen[$k] / $el)
}
""
"total {0:0.000} CPU-s = {1:0.00} cores; {2:0.000} CPU-s/GB" -f $tot, ($tot/$el), ($tot/($size/1e9))
