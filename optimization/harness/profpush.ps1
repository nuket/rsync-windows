# Profile rsync.exe (or ssh.exe) through one push or pull.  Nothing here uses
# Select-Object -First on a live pipeline: that stops the upstream command,
# which quietly kills the transfer being measured.
param(
    [ValidateSet('push', 'pull')] [string] $Dir = 'push',
    [string] $Watch = 'rsync.exe',
    [int] $Seconds = 12,
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-rwdi-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-rwdi-x64\ssh.exe'
)

$sp    = $PSScriptRoot
$host_ = 'claude@169.254.238.153'
$src   = "$sp\perf\big.bin"
$size  = (Get-Item $src).Length

& $Ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
if ($Dir -eq 'pull') {
    # put it there first, then time the way back
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" $src "${host_}:/tmp/perf/" | Out-Null
    $local = "$sp\pullback"
    New-Item -ItemType Directory -Force $local | Out-Null
    if (Test-Path "$local\big.bin") { [IO.File]::Delete("$local\big.bin") }
}

$prof = "$sp\prof-$Dir-$($Watch -replace '\.exe$','').txt"
$p = Start-Process -FilePath "$sp\sampler.exe" -ArgumentList $Watch, $Seconds, 500 `
        -RedirectStandardOutput $prof -RedirectStandardError "$prof.err" -PassThru -NoNewWindow
Start-Sleep -Milliseconds 250

$t0 = Get-Date
if ($Dir -eq 'push') {
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" $src "${host_}:/tmp/perf/" | Out-Null
} else {
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" "${host_}:/tmp/perf/big.bin" "$sp\pullback\" | Out-Null
}
$el = ((Get-Date) - $t0).TotalSeconds
"{0} {1}: {2:0} MB/s ({3:0.00}s)" -f $Dir, $Watch, ($size / $el / 1e6), $el

while (-not $p.HasExited) { Start-Sleep -Milliseconds 200 }
Get-Content $prof
