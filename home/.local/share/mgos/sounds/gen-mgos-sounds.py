#!/usr/bin/env python3
# Генератор звукової теми mgOS (freedesktop sound theme): м'які «скляні» тони — щось середнє між macOS і Windows 11.
# v4: події, що були вимкнені, + попередження/помилка — у стилі Windows.
# Без зовнішніх бібліотек: синтез у чистому Python → WAV → .oga (ffmpeg/vorbis).
#   gen-mgos-sounds.py [тека_теми]      (за замовчуванням ~/.local/share/sounds/mgos)
import math, os, random, struct, subprocess, sys, tempfile, wave

SR = 48000
OUT = next((a for a in sys.argv[1:] if not a.startswith("--")), None) or os.path.expanduser("~/.local/share/sounds/mgos")
random.seed(7)


def note(n):  # MIDI → Гц
    return 440.0 * 2 ** ((n - 69) / 12)


def silence(sec):
    return [0.0] * int(SR * sec)


def mix(dst, src, at=0.0, gain=1.0):
    o = int(at * SR)
    if len(dst) < o + len(src):
        dst.extend([0.0] * (o + len(src) - len(dst)))
    for i, v in enumerate(src):
        dst[o + i] += v * gain
    return dst


def bell(freq, dur=0.9, decay=5.0, bright=1.0, attack=0.004):
    """«Скляний» дзвіночок: синус + м'які негармонічні обертони, як у macOS; швидка атака, експоненційне згасання."""
    parts = [(1.0, 1.0, 1.0), (2.0, 0.28 * bright, 1.7), (2.76, 0.12 * bright, 2.6), (5.4, 0.05 * bright, 4.2)]
    n = int(SR * dur)
    out = [0.0] * n
    for mul, amp, dk in parts:
        f = freq * mul
        if f > SR / 2.2:
            continue
        w = 2 * math.pi * f / SR
        for i in range(n):
            t = i / SR
            env = (min(1.0, t / attack)) * math.exp(-decay * dk * t)
            out[i] += amp * env * math.sin(w * i)
    return out


def marimba(freq, dur=0.5, decay=9.0):
    """Дерев'яний «тап» (Windows 11-подібний): основний тон + 4-та гармоніка, дуже коротка атака."""
    n = int(SR * dur)
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        env = min(1.0, t / 0.002) * math.exp(-decay * t)
        out[i] = env * (math.sin(2 * math.pi * freq * t) + 0.18 * math.sin(2 * math.pi * freq * 4 * t) * math.exp(-20 * t))
    return out


def pad(freqs, dur=2.2, attack=0.35, release=1.2):
    """М'який акорд-підкладка для входу/виходу: злегка розстроєні синуси + повільний тремоло."""
    n = int(SR * dur)
    out = [0.0] * n
    for k, f in enumerate(freqs):
        for det in (-0.12, 0.12):
            ff = f * 2 ** (det / 12)
            ph = random.random() * 6.28
            w = 2 * math.pi * ff / SR
            for i in range(n):
                t = i / SR
                a = min(1.0, t / attack) if t < attack else 1.0
                r = 1.0 if t < dur - release else max(0.0, (dur - t) / release)
                out[i] += 0.5 * a * r * r * math.sin(w * i + ph) * (0.9 + 0.1 * math.sin(2 * math.pi * 0.7 * t + k))
    return out


def noise_burst(dur, lp=0.25, decay=40.0, attack=0.001):
    """Відфільтрований шум (клац затвора, папір)."""
    n = int(SR * dur)
    out, y = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        x = random.uniform(-1, 1)
        y += lp * (x - y)
        out[i] = y * min(1.0, t / attack) * math.exp(-decay * t)
    return out


def sweep(f0, f1, dur, decay=4.0):
    n = int(SR * dur)
    out, ph = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        f = f0 * (f1 / f0) ** (t / dur)
        ph += 2 * math.pi * f / SR
        out[i] = min(1.0, t / 0.01) * math.exp(-decay * t) * (math.sin(ph) + 0.15 * math.sin(2 * ph))
    return out


def reverb(x, wet=0.22, room=0.78):
    """Простий реверб Шредера: 4 гребінчасті + 2 all-pass — дає «повітря» навколо дзвіночків."""
    tail = int(SR * 0.6)
    x = x + [0.0] * tail
    n = len(x)
    combs = [1557, 1617, 1491, 1422]
    acc = [0.0] * n
    for d in combs:
        buf = [0.0] * n
        for i in range(n):
            buf[i] = x[i] + (room * buf[i - d] if i >= d else 0.0)
            acc[i] += buf[i] * 0.25
    for d, g in ((225, 0.5), (556, 0.5)):
        y = [0.0] * n
        for i in range(n):
            xd = acc[i - d] if i >= d else 0.0
            yd = y[i - d] if i >= d else 0.0
            y[i] = -g * acc[i] + xd + g * yd
        acc = y
    return [x[i] * (1 - wet) + acc[i] * wet for i in range(n)]


def finish(x, peak=0.7, fade=0.02):
    m = max(1e-9, max(abs(v) for v in x))
    x = [v * peak / m for v in x]
    nf = int(SR * fade)
    for i in range(min(nf, len(x))):
        x[-1 - i] *= i / nf
    # обрізаємо тишу в кінці
    while len(x) > SR // 10 and abs(x[-1]) < 1e-4:
        x.pop()
    return x


def stereo_write(path, x, width=0.0):
    """Мінімальне стерео: правий канал затриманий на кілька семплів (ширина без фазових проблем)."""
    d = int(width * SR)
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        frames = bytearray()
        for i in range(len(x)):
            l = x[i]
            r = x[i - d] if i >= d else 0.0
            frames += struct.pack("<hh", int(max(-1, min(1, l)) * 32000), int(max(-1, min(1, r)) * 32000))
        w.writeframes(bytes(frames))


# ---------- інструменти v2 ----------
def kalimba(freq, dur=0.6, decay=7.0, shimmer=0.35):
    """Калімба: м'який «щипок» — основний тон + негармонічні 5.4× і 2.0× обертони, що швидко гаснуть."""
    n = int(SR * dur)
    out = [0.0] * n
    w = 2 * math.pi * freq / SR
    for i in range(n):
        t = i / SR
        env = min(1.0, t / 0.0015) * math.exp(-decay * t)
        out[i] = env * (math.sin(w * i) + shimmer * math.exp(-28 * t) * math.sin(5.4 * w * i)
                        + 0.22 * math.exp(-9 * t) * math.sin(2.0 * w * i))
    return out


def hp_noise(dur, decay=60.0, hp=0.6, attack=0.0008):
    """Світлий шум (high-pass): затвор, папір, «вжух»."""
    n = int(SR * dur)
    out, prev = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        x = random.uniform(-1, 1)
        y = x - hp * prev; prev = x
        out[i] = y * min(1.0, t / attack) * math.exp(-decay * t)
    return out


def whoosh(dur, up=True, gain=1.0):
    """Відфільтрований шум, у якого фільтр плавно відкривається (up) або закривається."""
    n = int(SR * dur)
    out, y = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        k = t / dur
        lp = 0.02 + 0.35 * (k if up else 1 - k)
        y += lp * (random.uniform(-1, 1) - y)
        out[i] = gain * y * math.sin(math.pi * k) ** 1.5
    return out


def thump(freq=70, dur=0.18):
    """Низький м'який «тум» — відчуття фізичного контакту (кабель, пристрій)."""
    n = int(SR * dur)
    out, ph = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        f = freq * (1 + 1.5 * math.exp(-40 * t))
        ph += 2 * math.pi * f / SR
        out[i] = min(1.0, t / 0.002) * math.exp(-22 * t) * math.sin(ph)
    return out


# ---------- самі звуки ----------
# ЛИШЕНО без змін (подобаються): попередження, помилка, гучність — s_warning, s_error, s_volume.
# Решта — v2: калімба/марімба в ля мажорі (A C# E F# B), щоб тема звучала однією «родиною».
def s_warning():          # попередження: два низхідні тони  [не змінювати]
    x = mix(marimba(note(76), 0.5), marimba(note(71), 0.6), at=0.13, gain=0.95)
    return reverb(x, 0.2)


def s_error():            # помилка: низький м'який подвійний «бонк»  [не змінювати]
    x = mix(marimba(note(62), 0.35, decay=14), marimba(note(57), 0.5, decay=11), at=0.12)
    return reverb(x, 0.15)


def s_volume():           # гучність v2: м'який приглушений дерев'яний «ток» — без дзвінкого обертону
    n = int(SR * 0.08)
    f = note(79)              # G5 (був C7 — надто дзвінко)
    x = [min(1.0, (i / SR) / 0.0015) * math.exp(-60 * i / SR) *
         (math.sin(2 * math.pi * f * i / SR) + 0.12 * math.exp(-90 * i / SR) * math.sin(2 * math.pi * 2 * f * i / SR))
         for i in range(n)]
    y = 0.0                   # м'який low-pass — «дерево під тканиною»
    for i in range(n):
        y += 0.35 * (x[i] - y); x[i] = y
    return silence(0.12) + finish(x, 0.42)   # 120 мс тиші: потік PipeWire встигає відкритись


# v3: усе — марімба, як улюблені «помилка / попередження / гучність»: 1–3 короткі тони, майже сухо.
def mseq(notes, step=0.11, decay=10.0, dur=0.5, gains=None, rv=0.18):
    x = []
    for k, n in enumerate(notes):
        mix(x, marimba(note(n), dur, decay=decay), at=step * k, gain=(gains[k] if gains else 1.0))
    return reverb(x, rv)


def s_message():          # сповіщення: A5 → E6, легко вгору
    return mseq((81, 88), step=0.1, decay=10, gains=(0.9, 1.0), rv=0.2)


def s_bell():             # інформація: один м'який E6
    return mseq((88,), decay=9, dur=0.6, rv=0.2)


def s_complete():         # готово: A5 C#6 E6 — швидко вгору
    return mseq((81, 85, 88), step=0.075, decay=11, gains=(0.85, 0.9, 1.0), rv=0.2)


def s_plug():             # зарядку підключено: E5 → A5
    return mseq((76, 81), step=0.12, decay=9, rv=0.18)


def s_unplug():           # зарядку відключено: A5 → E5
    return mseq((81, 76), step=0.12, decay=9, gains=(0.9, 0.85), rv=0.18)


def s_device_added():     # пристрій підключено: B5 → E6, коротко
    return mseq((83, 88), step=0.08, decay=13, dur=0.4, rv=0.15)


def s_device_removed():   # пристрій відключено: E6 → B5
    return mseq((88, 83), step=0.08, decay=13, dur=0.4, gains=(0.9, 0.85), rv=0.15)


def s_screenshot():       # знімок екрана: два крихітні «тіки» (родич звуку гучності)
    x = mix(marimba(note(100), 0.06, decay=70), marimba(note(96), 0.07, decay=60), at=0.07, gain=0.9)
    return silence(0.08) + reverb(x, 0.08)


def s_trash():            # кошик очищено: E6 C#6 A5 — швидко вниз, глухіше
    return mseq((88, 85, 81), step=0.06, decay=16, dur=0.35, gains=(0.9, 0.85, 0.8), rv=0.12)


def s_login():            # вхід: A4 C#5 E5 A5 E6 — повільне арпеджіо вгору
    return mseq((69, 73, 76, 81, 88), step=0.13, decay=5, dur=1.0, gains=(0.7, 0.75, 0.8, 0.9, 1.0), rv=0.3)


def s_logout():           # вихід: E6 A5 E5 C#5 A4 — вниз
    return mseq((88, 81, 76, 73, 69), step=0.12, decay=5.5, dur=0.9, gains=(0.9, 0.85, 0.8, 0.75, 0.75), rv=0.28)


def s_battery_low():      # низький заряд: два однакові низькі A4
    return mseq((69, 69), step=0.2, decay=8, dur=0.5, gains=(1.0, 0.8), rv=0.2)


def s_sent():             # надіслано: E6 → A6, дуже коротко і високо
    return mseq((88, 93), step=0.06, decay=15, dur=0.35, rv=0.15)



# ---------- v4: стиль Windows (власні синтезовані звуки, не копії) ----------
def synth(freq, dur=0.5, decay=6.0, harm=(1.0, 0.35, 0.15), attack=0.004):
    """Чистий «Windows-подібний» тон: синус + кілька гармонік, швидка атака, плавне згасання."""
    n = int(SR * dur)
    out = [0.0] * n
    for k, a in enumerate(harm, 1):
        w = 2 * math.pi * freq * k / SR
        for i in range(n):
            t = i / SR
            out[i] += a * min(1.0, t / attack) * math.exp(-decay * (1 + 0.4 * k) * t) * math.sin(w * i)
    return out


def buzz(freq, dur=0.3, decay=7.0):
    """Різкуватий «стоп»-тон для помилки: пилка з low-pass, трохи розстроєна пара."""
    n = int(SR * dur)
    out, y = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        s = 0.0
        for d in (1.0, 1.006):
            ph = (freq * d * t) % 1.0
            s += 2 * ph - 1
        y += 0.18 * (s * 0.5 - y)
        out[i] = y * min(1.0, t / 0.003) * math.exp(-decay * t)
    return out


def sseq(notes, step=0.12, dur=0.6, decay=6.0, gains=None, rv=0.2):
    x = []
    for k, n in enumerate(notes):
        mix(x, synth(note(n), dur, decay), at=step * k, gain=(gains[k] if gains else 1.0))
    return reverb(x, rv)


def s_warning_w():        # попередження: м'який чистий дзвінок, два тони вниз (як системні Windows 11 / macOS)
    x = mix(synth(note(79), 0.6, 6.0, (1.0, 0.18, 0.05), 0.008), synth(note(74), 0.8, 5.0, (1.0, 0.18, 0.05), 0.008), at=0.15, gain=0.95)
    return reverb(x, 0.18)


def s_error_w():          # помилка: округлий низький «дум-дум» вниз без різкості (як Basso / Critical Stop)
    x = mix(synth(note(57), 0.7, 5.5, (1.0, 0.3, 0.06), 0.01), synth(note(50), 1.0, 4.5, (1.0, 0.3, 0.06), 0.01), at=0.16)
    mix(x, thump(60, 0.25), at=0.16, gain=0.35)
    return reverb(x, 0.16)


def tine(freq, dur=0.8, decay=4.5, idx=1.6, bright=1.0):
    """«Електропіаніно»-тембр (FM, як у системних звуках macOS/Windows): тепле тіло + короткий дзвінкий удар, легкий хорус."""
    n = int(SR * dur)
    out = [0.0] * n
    for det, g in ((1.0, 0.6), (1.0035, 0.4)):
        f = freq * det
        w = 2 * math.pi * f / SR
        ph = random.random() * 6.28
        for i in range(n):
            t = i / SR
            env = min(1.0, t / 0.006) * math.exp(-decay * t)
            m = idx * bright * math.exp(-7 * t) * math.sin(2 * w * i + ph)       # модулятор 2:1 — дзвінкий удар
            out[i] += g * env * math.sin(w * i + m + 0.35 * math.sin(w * i) * math.exp(-3 * t))
    y = 0.0                                                                       # м'який low-pass: прибирає «цифрову» різкість
    for i in range(n):
        y += 0.55 * (out[i] - y); out[i] = y
    return out


def tseq(notes, step=0.12, dur=0.9, decay=4.5, gains=None, rv=0.25, idx=1.6):
    x = []
    for k, nt in enumerate(notes):
        mix(x, tine(note(nt), dur, decay, idx), at=step * k, gain=(gains[k] if gains else 1.0))
    return reverb(x, rv)


def warm_pad(freqs, dur, attack, release, gain=0.5):
    """Тепла підкладка: пилка з сильним low-pass (замість «чистих» синусів, що звучать як чіптюн)."""
    n = int(SR * dur)
    out = [0.0] * n
    for f in freqs:
        for det in (-0.07, 0.0, 0.07):
            ff = f * 2 ** (det / 12)
            y = 0.0
            for i in range(n):
                t = i / SR
                ph = (ff * t) % 1.0
                y += 0.035 * ((2 * ph - 1) - y)
                a = min(1.0, t / attack)
                r = 1.0 if t < dur - release else max(0.0, (dur - t) / release)
                out[i] += gain * y * a * a * r * r
    return out


def s_complete_w():       # готово: два теплі дзвінки вгору («тінь-тінь»)
    return tseq((79, 86), step=0.11, dur=1.0, decay=4.0, gains=(0.75, 1.0), rv=0.28)


def s_login_w():          # вхід: ширший теплий акорд-підйом + м'який дзвінок зверху
    x = warm_pad([note(50), note(57), note(62), note(66)], 3.2, 0.9, 1.8, 1.0)
    mix(x, tseq((74, 81, 86), step=0.28, dur=1.6, decay=2.8, gains=(0.55, 0.7, 0.9), rv=0.0, idx=1.2), at=0.7, gain=0.9)
    return reverb(x, 0.35)


def s_logout_w():         # вихід: те саме вниз і тихіше
    x = warm_pad([note(47), note(54), note(59), note(62)], 2.6, 0.4, 1.6, 0.9)
    mix(x, tseq((86, 81, 74), step=0.22, dur=1.3, decay=3.4, gains=(0.8, 0.65, 0.5), rv=0.0, idx=1.2), at=0.0, gain=0.85)
    return reverb(x, 0.33)


def s_sent_w():           # надіслано: м'який «вжух» і один чистий дзвінок
    x = whoosh(0.22, up=True, gain=0.35)
    mix(x, tine(note(88), 0.7, 5.0, 1.2), at=0.12, gain=0.9)
    return reverb(x, 0.2)


def s_screenshot_w():     # знімок екрана: приглушений м'який «клік» затвора (без різкого шуму)
    a = noise_burst(0.035, lp=0.18, decay=110.0, attack=0.0015)
    x = mix(a, tine(note(72), 0.12, 38.0, 0.6), gain=0.7)
    mix(x, noise_burst(0.05, lp=0.14, decay=70.0, attack=0.002), at=0.07, gain=0.75)
    return silence(0.05) + reverb(x, 0.12)


def s_trash_w():          # кошик очищено: м'який шурхіт паперу, що стихає
    n = int(SR * 0.5)
    x, y = [0.0] * n, 0.0
    for i in range(n):
        t = i / SR
        burst = 1.0 if random.random() < 0.0035 else 0.0
        y += 0.22 * (random.uniform(-1, 1) * (0.25 + 4 * burst) - y)
        x[i] = y * min(1.0, t / 0.01) * math.exp(-5.5 * t)
    mix(x, tine(note(64), 0.35, 8.0, 0.8), at=0.0, gain=0.12)
    return reverb(x, 0.12)


def _bt(kind, up):
    """Варіанти звуків Bluetooth: 1 «Дзвіночок», 2 «Краплі», 3 «Свист», 4 «Дерево», 5 «Акорд»."""
    x = silence(1.4)
    F = lambda m: 440 * 2 ** ((m - 69) / 12)
    seq = (79, 86) if up else (86, 79)
    if kind == 1:   # скляні дзвіночки, як AirPods/macOS
        mix(x, bell(F(seq[0] + 12), dur=1.2, decay=3.2), 0.0, 0.7)
        mix(x, bell(F(seq[1] + 12), dur=1.3, decay=3.0), 0.16, 0.8)
        return reverb(x, wet=0.28)
    if kind == 2:   # калімба-краплі, три ноти
        for k, m in enumerate((seq[0], (seq[0] + seq[1]) // 2 + 1, seq[1]) if up else (seq[0], (seq[0] + seq[1]) // 2 - 1, seq[1])):
            mix(x, kalimba(F(m + 12), dur=0.7, decay=6.0), 0.09 * k, 0.8)
        return reverb(x, wet=0.25)
    if kind == 3:   # м'який «свист» угору/вниз + тихий дзвін
        mix(x, sweep(500, 1500, 0.22, decay=9.0) if up else sweep(1500, 500, 0.22, decay=9.0), 0.0, 0.5)
        mix(x, bell(F(91 if up else 79), dur=1.0, decay=4.0), 0.16 if up else 0.12, 0.55)
        return reverb(x, wet=0.2)
    if kind == 4:   # дерев'яна маримба, два «токи»
        mix(x, marimba(F(seq[0]), 0.5, decay=8.0), 0.0, 0.9)
        mix(x, marimba(F(seq[1]), 0.6, decay=8.0), 0.11, 1.0)
        return reverb(x, wet=0.15)
    ch = (74, 79, 83) if up else (83, 79, 74)   # 5: арпеджіо мажорного тризвуку
    for k, m in enumerate(ch):
        mix(x, tine(F(m + 12), dur=0.9, decay=4.5, idx=0.7), 0.07 * k, 0.65)
    return reverb(x, wet=0.22)


def s_bt_c2(): return _bt(2, True)
def s_bt_c4(): return _bt(4, True)
def s_bt_d2(): return _bt(2, False)
def s_bt_d4(): return _bt(4, False)


SOUNDS = {
    # freedesktop-імена (їх шукають GTK, libcanberra, Firefox, dunst-скрипти тощо) → генератор, ширина стерео
    "message-new-instant": (s_message, 0.0004), "message": (s_message, 0.0004),
    "bell": (s_bell, 0.0003), "dialog-information": (s_bell, 0.0003),
    "dialog-warning": (s_warning_w, 0.0003), "dialog-error": (s_error_w, 0.0002),
    "complete": (s_complete_w, 0.0005), "power-plug": (s_plug, 0.0003), "power-unplug": (s_unplug, 0.0003),
    "bt-c2": (s_bt_c2, 0.0004), "bt-d2": (s_bt_d2, 0.0004), "bt-c4": (s_bt_c4, 0.0004), "bt-d4": (s_bt_d4, 0.0004), 
    "device-added": (s_device_added, 0.0003), "device-removed": (s_device_removed, 0.0003),
    "audio-volume-change": (s_volume, 0.0), "screen-capture": (s_screenshot_w, 0.0002), "camera-shutter": (s_screenshot_w, 0.0002),
    "trash-empty": (s_trash_w, 0.0004), "desktop-login": (s_login_w, 0.0006), "desktop-logout": (s_logout_w, 0.0006),
    "battery-low": (s_battery_low, 0.0003), "battery-caution": (s_battery_low, 0.0003),
    "message-sent-instant": (s_sent_w, 0.0003), "outgoing": (s_sent_w, 0.0003),
}


KEEP = set()   # ці звуки не перегенеровуються (лише з --all)


def main():
    d = os.path.join(OUT, "stereo")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(OUT, "index.theme"), "w") as f:
        f.write("[Sound Theme]\nName=mgOS\nComment=mgOS — м'які скляні звуки\nInherits=freedesktop\nDirectories=stereo\n\n"
                "[stereo]\nOutputProfile=stereo\n")
    cache = {}
    tmp = tempfile.mkdtemp(prefix="mgos-snd-")
    only = next((a.split("=", 1)[1].split(",") for a in sys.argv if a.startswith("--only=")), None)
    for name, (fn, width) in SOUNDS.items():
        if only and name not in only:
            continue
        if fn in KEEP and os.path.exists(os.path.join(d, name + ".oga")) and "--all" not in sys.argv:
            print("= лишаю", name); continue
        if fn not in cache:
            cache[fn] = finish(fn())
        wav = os.path.join(tmp, name + ".wav")
        stereo_write(wav, cache[fn], width)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", wav, "-c:a", "libvorbis", "-q:a", "5",
                        os.path.join(d, name + ".oga")], check=True)
        os.remove(wav)
        print("✓", name)
    os.rmdir(tmp)


if __name__ == "__main__":
    main()
