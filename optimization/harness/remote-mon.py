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
