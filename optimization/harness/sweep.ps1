# sweep.ps1 -- A/B one environment variable across several values, both
# directions, interleaved within a round so thermal drift lands on every arm,
# and cooled to a fixed temperature between rounds so no round inherits the
# previous one's heat.
param(
    [string]   $EnvName = 'SSH_SHMIO_SPIN',
    [string[]] $Values  = @('0','128','512','2048'),
    [string[]] $Dirs    = @('push','pull'),
    [int]      $Rounds  = 3,
    [int]      $CoolToC = 62,
    [int]      $CoolMaxSec = 240
)
$sp = $PSScriptRoot

function Get-TempC {
    try {
        $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction Stop |
             Where-Object { $_.ElementName -like '*CPU Probe*' } | Select-Object -First 1
        if ($s) { return [int]$s.CurrentReading }
    } catch {}
    return $null
}
function Cool {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $CoolMaxSec) {
        $t = Get-TempC
        if ($null -eq $t -or $t -le $CoolToC) { break }
        Start-Sleep -Seconds 5
    }
    "  [cooled to $(Get-TempC) C]"
}

$before = @(Import-Csv "$sp\runs.csv" -ErrorAction SilentlyContinue).Count
for ($r = 1; $r -le $Rounds; $r++) {
    Cool
    foreach ($d in $Dirs) {
        foreach ($v in $Values) {
            & "$sp\runx.ps1" -Tag "$EnvName=$v" -Dir $d -Src "$sp\perf\big.bin" `
                             -SetEnv "$EnvName=$v" -Note "sweep r$r"
        }
    }
}

# summarise just the rows this sweep added
$all = @(Import-Csv "$sp\runs.csv")
$mine = $all[$before..($all.Count-1)]
''
'{0,-24} {1,-5} {2,8} {3,10} {4,10} {5,8}' -f 'arm','dir','MB/s','rsyncCPU','cpu/GB','tempMax'
foreach ($d in $Dirs) {
    foreach ($v in $Values) {
        $g = $mine | Where-Object { $_.tag -eq "$EnvName=$v" -and $_.dir -eq $d }
        if (-not $g) { continue }
        '{0,-24} {1,-5} {2,8:0} {3,10:0.00} {4,10:0.000} {5,8:0}' -f "$EnvName=$v", $d,
            ($g | Measure-Object MBps -Average).Average,
            ($g | Measure-Object rsyncCPUs -Average).Average,
            ($g | Measure-Object cpuPerGB -Average).Average,
            ($g | Measure-Object tempMax -Average).Average
    }
}
