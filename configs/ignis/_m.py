import shutil
P="/tmp/claude-1000/-home-doki/94fb85d7-73d1-4821-8b59-1479f11dcea9/scratchpad/pub/configs/ignis/settings.py"
L="/home/doki/.config/ignis/settings.py"
shutil.copy2(L,"/home/doki/.config/ignis/_bak_settings.py")
pub=open(P).read().split("\n"); live=open(L).read().split("\n")
def pr(a,b): return pub[a-1:b]          # 1-based inclusive
def idx(pred):
    r=[i for i,l in enumerate(live) if pred(l)]
    assert len(r)==1,(len(r))
    return r[0]
# 1 import
i=idx(lambda l:l.startswith("from PyQt6.QtCore import (Qt, QEvent,QPropertyAnimation"))
live[i]=live[i].replace("QEvent,QPropertyAnimation","QEvent, QProcess,QPropertyAnimation")
# робимо знизу вгору, щоб індекси не збивались
i=idx(lambda l:l.strip()=="# --- Вигляд ---")
live[i:i]=pr(4675,4731)
i=idx(lambda l:"dashboard=self.create_dashboard_page)" in l)
live[i]=live[i].replace("dashboard=self.create_dashboard_page)","dashboard=self.create_dashboard_page, mgshare=self.create_mgshare_page)")
i=idx(lambda l:l.strip().startswith('("bluetooth", '))
live[i+1:i+1]=[pub[3925]]
assert "mgshare" in pub[3925]
i=idx(lambda l:l.startswith("# (індекс сторінки"))
live[i:i]=pr(3839,3922)
i=idx(lambda l:l.startswith("class SettingRow(QWidget)"))
live[i:i]=pr(2619,2888)
t="\n".join(live).replace("self._mgshare_timer.setInterval(5000)","self._mgshare_timer.setInterval(8000)")
open(L,"w").write(t)
