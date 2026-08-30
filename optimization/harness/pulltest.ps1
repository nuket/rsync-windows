param([string]$Dest = 'C:\Temp\vmtest', [int]$Secs = 15, [string]$Extra = '')
if (Test-Path $Dest) { Get-ChildItem $Dest -Recurse -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $Dest | Out-Null
$argline = "-rt -s --info=progress2 $Extra ""max@192.168.178.150:VirtualBox VMs/webos-2.28.0/"" ""$Dest/"""
Write-Output "ARGS: $argline"
$p = Start-Process -FilePath C:\tools\rsync\rsync.exe -ArgumentList $argline -NoNewWindow -PassThru -RedirectStandardOutput C:\Temp\pull.log -RedirectStandardError C:\Temp\pull.err
$sw = [Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Secs
$bytes = (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object Length -Sum).Sum
$sw.Stop()
if (-not $p.HasExited) { $p | Stop-Process -Force }
Start-Sleep -Milliseconds 800
$lines = (Get-Content C:\Temp\pull.log -Raw) -replace "`r","`n" -split "`n"
$lines | Where-Object { $_ -match '%' } | Select-Object -Last 2
Write-Output ("DISK: {0:N0} bytes in {1:N1}s = {2:N1} MB/s" -f $bytes, $sw.Elapsed.TotalSeconds, ($bytes/1MB/$sw.Elapsed.TotalSeconds))
Write-Output "--- stderr ---"
Get-Content C:\Temp\pull.err -Raw
