param([int]$Seconds = 90, [string]$Out)
$prev = @{}
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $line = (Get-Date).ToString('HH:mm:ss')
    foreach ($p in (Get-Process -Name rsync, sshd, ssh -ErrorAction SilentlyContinue)) {
        $k = "$($p.Name)#$($p.Id)"
        $cpu = $p.TotalProcessorTime.TotalSeconds
        if ($prev.ContainsKey($k)) { $line += ("  {0}={1}%" -f $k, [math]::Round(($cpu - $prev[$k]) * 100)) }
        $prev[$k] = $cpu
    }
    Add-Content -Path $Out -Value $line
    Start-Sleep -Milliseconds 1000
}
