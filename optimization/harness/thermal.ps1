# One backend, held at line rate long enough to get hot.
#
#   thermal.ps1 <cng|isal> <seconds> [cipher]
#
# Waits for the CPU to come back down to a fixed temperature first, so both
# backends start from the same place rather than one inheriting the other's
# heat.  Samples once a second: achieved throughput (from spew, which is
# feeding ssh), effective clock as a percentage of base, CPU temperature and
# fan speed.  Writes a CSV per run.
param(
    [Parameter(Mandatory = $true)][ValidateSet('cng', 'isal')] [string] $Backend,
    [int] $Seconds = 180,
    [string] $Cipher = 'aes128-gcm@openssh.com',
    [int] $StartTempC = 60,
    [int] $CoolMaxSec = 300
)

$ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
$host_ = 'claude@169.254.238.153'
$spew  = "$PSScriptRoot\spew.exe"
$csv   = "$PSScriptRoot\thermal-$Backend.csv"

function Get-Thermals {
    $s = Get-CimInstance -Namespace root/dcim/sysman -ClassName DCIM_NumericSensor -ErrorAction SilentlyContinue
    # ElementName is "Temperature Sensor:CPU Probe", not "CPU Probe"
    $cpu = $s | Where-Object { $_.ElementName -like '*CPU Probe*' } | Select-Object -First 1
    $fan = $s | Where-Object { $_.ElementName -like '*Fan*' } | Select-Object -First 1
    [pscustomobject]@{
        TempC = if ($cpu) { [int]$cpu.CurrentReading } else { $null }
        RPM   = if ($fan) { [int]$fan.CurrentReading } else { $null }
    }
}

# --- cool down, so both runs start alike --------------------------------
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $CoolMaxSec) {
    $th = Get-Thermals
    if ($th.TempC -eq $null -or $th.TempC -le $StartTempC) { break }
    Start-Sleep -Seconds 5
}
$start = Get-Thermals
"cooled to $($start.TempC) C, fan $($start.RPM) rpm; starting $Backend for $Seconds s"

# --- the load ------------------------------------------------------------
$env:SSH_AESGCM_BACKEND = $Backend
$err = "$PSScriptRoot\spew-$Backend.err"
if (Test-Path $err) { Remove-Item $err -ErrorAction SilentlyContinue }
# A .bat rather than a cmd /c string: the pipeline, the redirect and three
# quoted paths do not survive two levels of quoting reliably.
$bat = Join-Path $PSScriptRoot "run-$Backend.bat"
@(
    '@echo off',
    "set SSH_AESGCM_BACKEND=$Backend",
    "`"$spew`" $Seconds 256 2>`"$err`" | `"$ssh`" -c $Cipher $host_ `"cat > /dev/null`""
) | Set-Content -Encoding ASCII $bat
$job = Start-Process -FilePath $bat -PassThru -NoNewWindow

$clock = New-Object Diagnostics.PerformanceCounter('Processor Information', '% Processor Performance', '_Total')
$null = $clock.NextValue()
$rows = @()
$s0 = Get-Date
$deadline = (Get-Date).AddSeconds($Seconds + 30)
while (-not $job.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1000
    $th = Get-Thermals
    $rows += [pscustomobject]@{
        t       = [math]::Round(((Get-Date) - $s0).TotalSeconds, 1)
        clockPct= [math]::Round($clock.NextValue(), 1)
        tempC   = $th.TempC
        rpm     = $th.RPM
    }
}
$env:SSH_AESGCM_BACKEND = $null

# --- join the throughput series spew wrote -------------------------------
$rate = @{}
foreach ($line in (Get-Content $err -ErrorAction SilentlyContinue)) {
    if ($line -match '^([\d.]+),(\d+)$') { $rate[[math]::Round([double]$matches[1])] = [int]$matches[2] }
}
$out = foreach ($r in $rows) {
    [pscustomobject]@{
        t = $r.t; MBps = $rate[[math]::Round($r.t)]
        clockPct = $r.clockPct; tempC = $r.tempC; rpm = $r.rpm
    }
}
$out | Export-Csv -NoTypeInformation $csv

$have = $out | Where-Object { $_.MBps }
$n = $have.Count
if ($n -ge 10) {
    $first = ($have | Select-Object -First ([int]($n / 5)) | Measure-Object MBps -Average).Average
    $last  = ($have | Select-Object -Last  ([int]($n / 5)) | Measure-Object MBps -Average).Average
    "{0}: first fifth {1:0} MB/s, last fifth {2:0} MB/s ({3:+0.0;-0.0}%), temp {4} -> {5} C, clock {6:0}% -> {7:0}%" -f `
        $Backend, $first, $last, (100 * ($last - $first) / $first),
        ($have | Select-Object -First 1).tempC, ($have | Select-Object -Last 1).tempC,
        ($have | Select-Object -First ([int]($n / 5)) | Measure-Object clockPct -Average).Average,
        ($have | Select-Object -Last ([int]($n / 5)) | Measure-Object clockPct -Average).Average
}
"csv: $csv"
