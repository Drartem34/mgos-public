t=open('/home/doki/.config/ignis/_diff_settings.py.txt').read().splitlines()
cur=None;out=[]
for l in t:
    if l.startswith('@@'):
        if cur: out.append(cur)
        cur=[l,0,0]
    elif cur and l.startswith('-'): cur[1]+=1
    elif cur and l.startswith('+'): cur[2]+=1
out.append(cur)
for h,r,a in out: print(h,'-%d +%d'%(r,a))
