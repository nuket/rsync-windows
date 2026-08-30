param([string]$Backoff='', [string]$Extra='', [int]$Secs=16,
      [string]$Exe='C:\Users\Claude\devsrc\rsync-windows\build\rsync.exe',
      [string]$Cipher='aes128-gcm@openssh.com', [string]$Tag='')
function TrueSize($dir) {
  $sum = 0
  foreach ($f in (Get-ChildItem $dir -Recurse -File -Force -ErrorAction SilentlyContinue)) {
    try {
      $fs = [System.IO.File]::Open($f.FullName,'Open','Read','ReadWrite')
      $sum += $fs.Length; $fs.Close()
    } catch { $sum += $f.Length }
  }
  return $sum
}
$Dest='C:\Temp\vmtest'
if (Test-Path $Dest) { Get-ChildItem $Dest -Recurse -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $Dest | Out-Null
if ($Backoff) { $env:RSYNC_W32_BACKOFF = $Backoff } else { Remove-Item Env:RSYNC_W32_BACKOFF -ErrorAction SilentlyContinue }
$argline = "-rt -s $Extra -e ""ssh -c $Cipher"" ""max@192.168.178.150:VirtualBox VMs/Windows 10/"" ""$Dest/"""
$p = Start-Process -FilePath $Exe -ArgumentList $argline -NoNewWindow -PassThru -RedirectStandardOutput C:\Temp\pull.log -RedirectStandardError C:\Temp\pull.err
Start-Sleep -Seconds 6
$snap=@{}; foreach ($n in 'rsync','ssh') { foreach ($q in (Get-Process -Name $n -ErrorAction SilentlyContinue)) { $snap[$q.Id]=@($q,$q.TotalProcessorTime) } }
$f0 = TrueSize $Dest
$sw=[Diagnostics.Stopwatch]::StartNew(); Start-Sleep -Seconds $Secs
$f1 = TrueSize $Dest; $sw.Stop()
$cpu = foreach ($k in $snap.Keys) { $q=$snap[$k][0]; try { $q.Refresh(); "{0}:{1:N0}%" -f $q.ProcessName,(100*($q.TotalProcessorTime-$snap[$k][1]).TotalSeconds/$sw.Elapsed.TotalSeconds) } catch {} }
if (-not $p.HasExited) { $p | Stop-Process -Force }
"{0,-14} {1,-12} {2,7:N1} MB/s   cpu {3}" -f ($(if($Tag){$Tag}elseif($Backoff){$Backoff}else{'default'})), $(if($Extra){$Extra}else{'-'}), (($f1-$f0)/1MB/$sw.Elapsed.TotalSeconds), ($cpu -join ' ')
$e = Get-Content C:\Temp\pull.err -Raw; if ($e) { "  stderr: $e" }
