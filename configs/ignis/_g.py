import subprocess as s
def g(d,*a):
    return s.run(["git","-C",d]+list(a),capture_output=True,text=True).stdout
P="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub"
R="/home/doki/.local/share/mgos/recovery"
print(g(P,"log","--oneline","-8","--","configs/ignis/settings.py"))
print(g(P,"log","--oneline","-3"))
print(g(R,"log","--oneline","-6","--","configs/ignis/settings.py"))
print(g(R,"log","--oneline","-1"))
