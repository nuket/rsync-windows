param([string]$Exe='C:\Users\Claude\devsrc\rsync-windows\build\rsync.exe',
      [string]$Src='VirtualBox VMs/webos-2.28.0/',
      [int]$Secs=15, [string]$Extra='')
$Dest='C:\Temp\vmtest'
if (Test-Path $Dest) { Get-ChildItem $Dest -Recurse -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $Dest | Out-Null
$argline = "-rt -s $Extra ""max@192.168.178.150:$Src"" ""$Dest/"""
$p = Start-Process -FilePath $Exe -ArgumentList $argline -NoNewWindow -PassThru -RedirectStandardOutput C:\Temp\pull.log -RedirectStandardError C:\Temp\pull.err
Start-Sleep -Seconds 3
$sw=[Diagnostics.Stopwatch]::StartNew()
$b0 = 0; $ph=@{}
foreach ($n in 'rsync','ssh') { foreach ($q in (Get-Process -Name $n -ErrorAction SilentlyContinue)) { $ph[$q.Id]=@($q,$q.TotalProcessorTime) } }
Start-Sleep -Seconds $Secs
$sw.Stop()
$cpu = foreach ($k in $ph.Keys) { $q=$ph[$k][0]; try { $q.Refresh(); "{0}={1:N0}%" -f $q.ProcessName, (100*($q.TotalProcessorTime-$ph[$k][1]).TotalSeconds/$sw.Elapsed.TotalSeconds) } catch {} }
if (-not $p.HasExited) { $p | Stop-Process -Force }
Start-Sleep -Milliseconds 800
$tot = (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object Length -Sum).Sum
$rate = $tot/1MB/($sw.Elapsed.TotalSeconds+3)
"RESULT: {0:N0} bytes / {1:N1}s = {2:N1} MB/s  ({3:N0} Mbps)  cpu: {4}" -f $tot,($sw.Elapsed.TotalSeconds+3),$rate,($rate*8),($cpu -join ' ')
$e = Get-Content C:\Temp\pull.err -Raw
if ($e) { "STDERR: $e" }
