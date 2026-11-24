import subprocess as s
R="/home/doki/.local/share/mgos/recovery"
pub="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub/configs/ignis/settings.py"
tmp="/home/doki/.config/ignis/_base.py"
cs=s.run(["git","-C",R,"log","--format=%h","-150","--","configs/ignis/settings.py"],capture_output=True,text=True).stdout.split()
best=None
for c in cs:
    open(tmp,"w").write(s.run(["git","-C",R,"show",c+":configs/ignis/settings.py"],capture_output=True,text=True).stdout)
    o=s.run(["git","diff","--no-index","--numstat",tmp,pub],capture_output=True,text=True).stdout.split()
    a,d=int(o[0]),int(o[1])
    print(c,a,d)
    if best is None or d<best[1]: best=(c,d,a)
print("BEST",best)
