# xtrace.ps1 -- capture one push (or pull) under xperf, with sampled profile,
# context switches and ready-thread edges, then decode the reports.
#
# Needs elevation (NT Kernel Logger).  Writes <Name>.etl and <Name>-*.txt.
param(
    [string] $Name = 'push',
    [ValidateSet('push','pull')] [string] $Dir = 'push',
    [string] $Src = '',
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-rwdi-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-rwdi-x64\ssh.exe',
    [string] $Cipher = 'aes128-gcm@openssh.com',
    [int]    $ProfIntUnits = 5000,          # 100 ns units; 5000 = 0.5 ms
    # 'cpu'  -- sampled profile only, so the butterfly is a pure CPU profile
    # 'wait' -- adds CSwitch/ReadyThread, which swamps the butterfly with
    #           SwapContext but is what answers "who blocks on whom"
    [ValidateSet('cpu','wait')] [string] $Mode = 'cpu',
    [switch] $NoDecode
)

$ErrorActionPreference = 'Continue'
$sp    = $PSScriptRoot
$wpt   = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit'
$xperf = "$wpt\xperf.exe"
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$sshOpt = '-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new'
if (-not $Src) { $Src = "$sp\perf\big.bin" }
$size = (Get-Item $Src).Length
# the logger's own file and the merged file must not be the same path: merging
# onto the file the kernel logger just wrote produces an ETL with no events
$raw  = "$sp\$Name-raw.etl"
$etl  = "$sp\$Name.etl"

function Remote-Clean {
    & $sys @sshOpt $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null
}

# a stale session would make -on fail
& $xperf -stop 2>&1 | Out-Null
Remote-Clean
& $sys @sshOpt $rhost "mkdir -p /tmp/perf" 2>&1 | Out-Null

$leaf  = (Get-Item $Src).Name
$local = "$sp\pullback"
if ($Dir -eq 'pull') {
    & $Rsync -a --inplace -e "$Ssh -c $Cipher" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    New-Item -ItemType Directory -Force $local | Out-Null
    if (Test-Path "$local\$leaf") { [IO.File]::Delete("$local\$leaf") }
}

& $xperf -setprofint $ProfIntUnits 2>&1 | Out-Null
if ($Mode -eq 'cpu') { $flags = 'PROC_THREAD+LOADER+PROFILE'; $walk = 'Profile' }
else { $flags = 'PROC_THREAD+LOADER+PROFILE+CSWITCH+DISPATCHER'; $walk = 'Profile+CSwitch+ReadyThread' }
& $xperf -on $flags -stackwalk $walk `
         -BufferSize 1024 -MinBuffers 256 -MaxBuffers 1024 -f $raw 2>&1
if ($LASTEXITCODE -ne 0) { throw "xperf -on failed ($LASTEXITCODE)" }

$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Dir -eq 'push') {
    & $Rsync -a --inplace -e "$Ssh -c $Cipher" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
} else {
    & $Rsync -a --inplace -e "$Ssh -c $Cipher" "${rhost}:/tmp/perf/$leaf" "$local/" 2>&1 | Out-Null
}
$sw.Stop()
& $xperf -stop -d $etl 2>&1 | Out-Null

"{0} {1}: {2:0} MB/s ({3:0.00}s) -> {4}" -f $Name, $Dir, ($size/$sw.Elapsed.TotalSeconds/1e6), $sw.Elapsed.TotalSeconds, $etl

if ($Dir -eq 'pull' -and (Test-Path "$local\$leaf")) { [IO.File]::Delete("$local\$leaf") }
Remote-Clean
& $xperf -setprofint 10000 2>&1 | Out-Null

if ($NoDecode) { return }

$env:_NT_SYMBOL_PATH = "srv*$sp\symcache*https://msdl.microsoft.com/download/symbols;" +
    "C:\Users\Claude\devsrc\rsync-windows\build-rwdi-x64;" +
    "C:\Users\Claude\devsrc\rsync-windows\openssh\bin\x64\Release"
foreach ($a in 'profile','cswitch','readythread') {
    & $xperf -i $etl -o "$sp\$Name-$a.txt" -symbols -tle -tti -a $a 2>&1 | Out-Null
    $f = Get-Item "$sp\$Name-$a.txt" -ErrorAction SilentlyContinue
    "  $a -> $(if($f){'{0:N0} bytes' -f $f.Length}else{'FAILED'})"
}
foreach ($proc in 'rsync.exe','ssh.exe') {
    $o = "$sp\$Name-stack-$($proc -replace '\.exe$','').txt"
    & $xperf -i $etl -o $o -symbols -tle -tti -a stack -butterfly 20 -process $proc 2>&1 | Out-Null
    $f = Get-Item $o -ErrorAction SilentlyContinue
    "  stack/$proc -> $(if($f){'{0:N0} bytes' -f $f.Length}else{'FAILED'})"
}
