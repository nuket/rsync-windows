# remote.ps1 -- what is the Linux receiver doing during a push?
# Samples /proc for the remote rsync + sshd while a push runs, so we can see
# whether the far end is the thing setting the pace.
param(
    [ValidateSet('push','pull')] [string] $Dir = 'push',
    [string] $Src = '',
    [string] $Rsync = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\rsync.exe',
    [string] $Ssh   = 'C:\Users\Claude\devsrc\rsync-windows\build-x64\ssh.exe'
)
$sp    = $PSScriptRoot
$rhost = 'claude@169.254.238.153'
$sys   = 'C:\Windows\System32\OpenSSH\ssh.exe'
$o     = '-o','BatchMode=yes','-o','StrictHostKeyChecking=accept-new'
if (-not $Src) { $Src = "$sp\perf\big.bin" }
$size = (Get-Item $Src).Length

& $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"; mkdir -p /tmp/perf" 2>&1 | Out-Null

# sampler on the far side: per-process utime+stime deltas, 100 ms apart
$prog = @'
import time,os,glob,sys
def snap():
    out={}
    for p in glob.glob('/proc/[0-9]*/stat'):
        try:
            f=open(p).read()
            name=f[f.index('(')+1:f.rindex(')')]
            rest=f[f.rindex(')')+2:].split()
            if name in ('rsync','sshd','sshd-session'):
                out[p.split('/')[2]+':'+name]=(int(rest[11])+int(rest[12]))
        except Exception: pass
    return out
hz=os.sysconf('SC_CLK_TCK'); prev=snap(); t0=time.time()
while time.time()-t0 < 25:
    time.sleep(0.1); cur=snap()
    row=[]
    for k,v in cur.items():
        d=v-prev.get(k,v)
        if d>0: row.append('%s=%.0f%%'%(k,100.0*d/hz/0.1))
    if row: print('%.1f '%(time.time()-t0)+' '.join(row),flush=True)
    prev=cur
'@
# feed the script over stdin: nothing then has to survive two shells' quoting
$pyf = "$sp\remote-mon.py"
Set-Content -Path $pyf -Value $prog -Encoding ASCII
$log = "$sp\remote-cpu.txt"
$mon = Start-Process -FilePath $sys -ArgumentList (@($o) + @($rhost, 'python3 -')) `
        -PassThru -NoNewWindow -RedirectStandardInput $pyf `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
Start-Sleep -Milliseconds 800

$leaf  = (Get-Item $Src).Name
$local = "$sp\pullback"
if ($Dir -eq 'pull') {
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
    New-Item -ItemType Directory -Force $local | Out-Null
    if (Test-Path "$local\$leaf") { [IO.File]::Delete("$local\$leaf") }
}
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Dir -eq 'push') {
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" $Src "${rhost}:/tmp/perf/" 2>&1 | Out-Null
} else {
    & $Rsync -a --inplace -e "$Ssh -c aes128-gcm@openssh.com" "${rhost}:/tmp/perf/$leaf" "$local/" 2>&1 | Out-Null
}
$sw.Stop()
if ($Dir -eq 'pull' -and (Test-Path "$local\$leaf")) { [IO.File]::Delete("$local\$leaf") }
"{0}: {1:0} MB/s ({2:0.00}s)" -f $Dir, ($size/$sw.Elapsed.TotalSeconds/1e6), $sw.Elapsed.TotalSeconds
Start-Sleep -Milliseconds 400
try { if (-not $mon.HasExited) { $mon.Kill() } } catch {}
& $sys @o $rhost "python3 -c `"import shutil; shutil.rmtree('/tmp/perf', ignore_errors=True)`"" 2>&1 | Out-Null
""
"--- remote CPU while the push ran ---"
Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 40
