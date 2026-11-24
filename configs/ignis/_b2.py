import subprocess as s,difflib
R="/home/doki/.local/share/mgos/recovery"
pub="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub/configs/ignis/settings.py"
for c in ("999637ac","dff4faae"):
    open("/home/doki/.config/ignis/_base_%s.py"%c,"w").write(s.run(["git","-C",R,"show",c+":configs/ignis/settings.py"],capture_output=True,text=True).stdout)
A=open("/home/doki/.config/ignis/_base_999637ac.py").read().splitlines(1)
B=open(pub).read().splitlines(1)
sm=difflib.SequenceMatcher(None,A,B,autojunk=False)
with open("/home/doki/.config/ignis/_ops.txt","w") as f:
    for tag,i1,i2,j1,j2 in sm.get_opcodes():
        if tag=="equal": continue
        f.write("### %s base %d-%d pub %d-%d\n"%(tag,i1+1,i2,j1+1,j2))
        if tag in("delete","replace"):
            for l in A[i1:min(i2,i1+4)]: f.write("-"+l)
        if tag in("insert","replace"):
            for l in B[j1:min(j2,j1+3)]: f.write("+"+l)
