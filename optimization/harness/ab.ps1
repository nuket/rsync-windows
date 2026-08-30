# A/B two settings of one environment variable over the same push, alternating
# so the laptop's thermal drift lands on both.  Each run is logged as it lands.
#
#   ab.ps1 -EnvName RSYNC_WIN32_NO_MAPREAD -A '' -B '1' -Rounds 4
param(
    [Parameter(Mandatory = $true)][string] $EnvName,
    [string] $A = '', [string] $B = '1',
    [string] $ALabel = 'on', [string] $BLabel = 'off',
    [int] $Rounds = 4,
    [string] $Src = '',
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
)

$sp    = $PSScriptRoot
$host_ = 'claude@169.254.238.153'
if (-not $Src) { $Src = "$sp\perf\big.bin" }
$size  = (Get-Item $Src).Length

function Run-One($val, $label) {
    & $Ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
    if ($val -eq '') { Remove-Item "env:$EnvName" -ErrorAction SilentlyContinue }
    else { Set-Item "env:$EnvName" $val }
    $t0 = Get-Date
    $p = Start-Process -FilePath $Rsync -ArgumentList @(
        '-a', '--inplace', '-e', "`"$Ssh -c aes128-gcm@openssh.com`"", "`"$Src`"", "${host_}:/tmp/perf/"
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
    $el = ((Get-Date) - $t0).TotalSeconds
    if ($val -eq '') { Remove-Item "env:$EnvName" -ErrorAction SilentlyContinue }
    [pscustomobject]@{
        which    = $label
        MBps     = [math]::Round($size / $el / 1e6, 0)
        rsyncCPU = [math]::Round($rcpu, 2)
        sshCPU   = [math]::Round($scpu, 2)
        cpuPerGB = [math]::Round(($rcpu + $scpu) / ($size / 1e9), 3)
    }
}

$out = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    $out += Run-One $A $ALabel
    $out += Run-One $B $BLabel
}
& $Ssh $host_ "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" | Out-Null
$out | Export-Csv -NoTypeInformation "$sp\ab-$EnvName.csv"
$out | Format-Table -AutoSize
foreach ($l in $ALabel, $BLabel) {
    $g = $out | Where-Object which -eq $l
    "{0,-6}: {1,6:0} MB/s   rsync {2,5:0.00}s   {3:0.000} CPU-s/GB" -f $l,
        ($g | Measure-Object MBps -Average).Average,
        ($g | Measure-Object rsyncCPU -Average).Average,
        ($g | Measure-Object cpuPerGB -Average).Average
}
