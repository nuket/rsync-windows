param([int]$Seconds = 60, [string]$Out)
$prev = @{}
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $line = (Get-Date).ToString('HH:mm:ss')
    foreach ($p in (Get-Process -Name rsync, sshd -ErrorAction SilentlyContinue)) {
        $k = "$($p.Name)#$($p.Id)"
        $u = $p.UserProcessorTime.TotalSeconds; $s = $p.PrivilegedProcessorTime.TotalSeconds
        if ($prev.ContainsKey($k)) {
            $du = [math]::Round(($u - $prev[$k][0]) * 100); $ds = [math]::Round(($s - $prev[$k][1]) * 100)
            if ($du + $ds -gt 2) { $line += ("  {0} user={1}% kernel={2}%" -f $k, $du, $ds) }
        }
        $prev[$k] = @($u, $s)
    }
    Add-Content -Path $Out -Value $line
    Start-Sleep -Milliseconds 1000
}
