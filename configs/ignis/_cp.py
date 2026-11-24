import shutil,os
P="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub/"
for s,d in ((P+"configs/SYSui/clock_popover.h","/home/doki/.config/SYSui/clock_popover.h"),
            (P+"home/.local/bin/mgos-share","/home/doki/.local/bin/mgos-share")):
    shutil.copy2(d,"/home/doki/.config/ignis/_bak_"+os.path.basename(d))
    t=open(s).read()
    t=t.replace("time.sleep(20)\n            try:\n                if any(d.get(\"bg\")","time.sleep(30)\n            try:\n                if any(d.get(\"bg\")")
    open(d,"w").write(t)
    print(d,os.stat(d).st_mode)
