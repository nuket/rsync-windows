param([int]$Secs=12)
Start-Sleep -Seconds 5
$names = 'rsync','sshd','ssh'
$snap = @{}
foreach ($n in $names) { foreach ($q in (Get-Process -Name $n -ErrorAction SilentlyContinue)) { $snap[$q.Id] = @($q, $q.TotalProcessorTime) } }
$sw=[Diagnostics.Stopwatch]::StartNew(); Start-Sleep -Seconds $Secs; $sw.Stop()
$rows = foreach ($k in $snap.Keys) {
  $q = $snap[$k][0]
  try { $q.Refresh(); $d = ($q.TotalProcessorTime - $snap[$k][1]).TotalSeconds
        [pscustomobject]@{Proc=$q.ProcessName; Pid=$k; CorePct=[math]::Round(100*$d/$sw.Elapsed.TotalSeconds,1)} } catch {}
}
$rows | Where-Object CorePct -gt 1 | Sort-Object CorePct -Descending | Format-Table -AutoSize
