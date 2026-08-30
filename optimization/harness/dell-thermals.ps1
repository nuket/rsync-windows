<#
    dell-thermals.ps1 -- read Dell Command | Monitor's temperature sensors,
    and optionally let a non-administrator read them too.

    Dell Command | Monitor puts its providers in root/dcim/sysman and leaves
    the namespace readable by Administrators only, so an ordinary shell gets
    "Access to a CIM resource was not available to the client".

    Run elevated:

        powershell -ExecutionPolicy Bypass -File dell-thermals.ps1
        powershell -ExecutionPolicy Bypass -File dell-thermals.ps1 -GrantUser "$env:USERNAME"

    -GrantUser adds Enable Account + Remote Enable for that account on the
    namespace and everything under it; afterwards the first form works from
    an ordinary, non-elevated shell.
#>
[CmdletBinding()]
param(
    [string] $Namespace = 'root/dcim/sysman',
    [string] $GrantUser
)

function Read-Sensors {
    param([string] $Ns)

    $all = Get-CimInstance -Namespace $Ns -ClassName DCIM_NumericSensor -ErrorAction Stop
    # CIM SensorType 2 is Temperature; show those first, then anything else
    # that carries a reading, since Dell's naming varies by model.
    $temps = @($all | Where-Object { $_.SensorType -eq 2 })
    if (-not $temps) {
        Write-Host "no SensorType=2 sensors; showing every numeric sensor" -ForegroundColor Yellow
        $temps = @($all)
    }
    foreach ($s in $temps) {
        $mod = if ($null -ne $s.UnitModifier) { [int]$s.UnitModifier } else { 0 }
        $val = [double]$s.CurrentReading * [math]::Pow(10, $mod)
        '{0,-42} {1,8:N1}   (raw {2}, unit {3}, modifier {4})' -f `
            $s.ElementName, $val, $s.CurrentReading, $s.BaseUnits, $mod
    }
}

if ($GrantUser) {
    $sid = (New-Object System.Security.Principal.NTAccount($GrantUser)
           ).Translate([System.Security.Principal.SecurityIdentifier]).Value
    Write-Host "granting $GrantUser ($sid) read access on $Namespace ..."

    $get = Invoke-CimMethod -Namespace $Namespace -ClassName __SystemSecurity `
                            -MethodName GetSecurityDescriptor -ErrorAction Stop
    if ($get.ReturnValue -ne 0) { throw "GetSecurityDescriptor returned $($get.ReturnValue)" }
    $sd = $get.Descriptor

    if ($sd.DACL | Where-Object { $_.Trustee.SidString -eq $sid }) {
        Write-Host "  already has an entry; leaving it alone" -ForegroundColor Yellow
    } else {
        $trustee = New-CimInstance -ClassName __Trustee -Namespace $Namespace -ClientOnly `
                                   -Property @{ SidString = $sid }
        # 1 = WBEM_ENABLE (Enable Account), 0x20 = WBEM_REMOTE_ENABLE.
        # AceFlags 2 = CONTAINER_INHERIT_ACE, so subnamespaces inherit it.
        $ace = New-CimInstance -ClassName __ACE -Namespace $Namespace -ClientOnly `
                               -Property @{ AccessMask = 0x21
                                            AceFlags   = 0x2
                                            AceType    = 0
                                            Trustee    = [CimInstance]$trustee }
        $sd.DACL += $ace
        $set = Invoke-CimMethod -Namespace $Namespace -ClassName __SystemSecurity `
                                -MethodName SetSecurityDescriptor `
                                -Arguments @{ Descriptor = $sd } -ErrorAction Stop
        if ($set.ReturnValue -ne 0) { throw "SetSecurityDescriptor returned $($set.ReturnValue)" }
        Write-Host "  done -- a normal shell can now read $Namespace" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "temperature sensors in ${Namespace}:"
try {
    Read-Sensors -Ns $Namespace
} catch {
    Write-Host "  could not read: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  run this elevated, or grant access with -GrantUser `"$env:USERNAME`"" -ForegroundColor Red
    exit 1
}
