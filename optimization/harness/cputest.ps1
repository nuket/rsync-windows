$Dest='C:\Temp\vmtest'
if (Test-Path $Dest) { Get-ChildItem $Dest -Recurse -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $Dest | Out-Null
$argline = "-rt -s --info=progress2 ""max@192.168.178.150:VirtualBox VMs/webos-2.28.0/"" ""$Dest/"""
$p = Start-Process -FilePath C:\tools\rsync\rsync.exe -ArgumentList $argline -NoNewWindow -PassThru -RedirectStandardOutput C:\Temp\pull.log -RedirectStandardError C:\Temp\pull.err
Start-Sleep -Seconds 4
$procs = @()
foreach ($n in 'rsync','ssh') { $procs += Get-Process -Name $n -ErrorAction SilentlyContinue }
$t0 = @{}; foreach ($q in $procs) { $t0[$q.Id] = $q.TotalProcessorTime }
$f0 = (Get-ChildItem $Dest -Recurse -File -Force | ForEach-Object { try { (New-Object IO.FileInfo $_.FullName).Length } catch {0} } | Measure-Object -Sum).Sum
$sw=[Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds 10
$sw.Stop()
foreach ($q in $procs) { $q.Refresh(); $d = ($q.TotalProcessorTime - $t0[$q.Id]).TotalSeconds; "{0} pid {1}: cpu {2:N2}s over {3:N1}s = {4:N1}%" -f $q.ProcessName,$q.Id,$d,$sw.Elapsed.TotalSeconds,(100*$d/$sw.Elapsed.TotalSeconds) }
if (-not $p.HasExited) { $p | Stop-Process -Force }
Start-Sleep -Milliseconds 500
$f1 = (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object Length -Sum).Sum
"bytes delta approx: {0:N0} over {1:N1}s -> {2:N1} MB/s" -f ($f1-$f0), ($sw.Elapsed.TotalSeconds+4), (($f1)/1MB/($sw.Elapsed.TotalSeconds+4))
