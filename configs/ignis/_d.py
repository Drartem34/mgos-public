import difflib,sys
P="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub/"
pairs={"settings.py":("/home/doki/.config/ignis/settings.py",P+"configs/ignis/settings.py"),
"topbar.cpp":("/home/doki/.config/SYSui/topbar.cpp",P+"configs/SYSui/topbar.cpp"),
"clock_popover.h":("/home/doki/.config/SYSui/clock_popover.h",P+"configs/SYSui/clock_popover.h"),
"build.sh":("/home/doki/.config/SYSui/build.sh",P+"configs/SYSui/build.sh"),
"mgos-share":("/home/doki/.local/bin/mgos-share",P+"home/.local/bin/mgos-share")}
for n,(a,b) in pairs.items():
    try: A=open(a).read().splitlines(1)
    except Exception as e: A=[]
    B=open(b).read().splitlines(1)
    d=list(difflib.unified_diff(A,B,"live","pub",n=1))
    open("/home/doki/.config/ignis/_diff_"+n+".txt","w").writelines(d)
    print(n,len(A),len(B),len(d))
