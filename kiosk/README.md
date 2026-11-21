# kioskOS у інсталяторі mgOS

Окремий пункт інсталятора: енергоощадна kiosk-сесія Hyprland, перенесена 1 в 1 з kiosk-машини
(Samsung Galaxy Book, i5-1335U, Iris Xe). Не залежить від mgOS — ставиться на чистий Arch або поруч із mgOS.

## Співіснування з mgOS (обидві системи на одному ПК)

Економія kiosk **ніколи не діє поза kiosk-сесією**. Усе перемикає `kiosk-session` + `kioskos-mode` (`kiosk/session/`):

| Що | У kiosk-сесії | У mgOS | Якщо kiosk впав / ПК вимкнули з kiosk |
|---|---|---|---|
| Профіль живлення | power-saver (`powerprofilesctl launch` — утримання) | той, що обрав у mgOS | PPD сам знімає утримання, коли процес помирає; `kioskos-ppd-guard.service` повертає профіль при завантаженні |
| Firefox | RAM-кеш, VA-API, авто-сон вкладок, скрол (`/usr/lib/firefox/kioskos.cfg`, лише при `KIOSKOS=1`) | стандартні значення, той самий профіль | налаштування читаються при кожному старті — нічого не «залипає» |
| GTK-тема, іконки, шрифт, курсор, gsettings | Graphite, MacTahoe, SF Compact, Bibata-Ice | тема mgOS | `kioskos-guard.service` (при вході) повертає тему mgOS |
| Nautilus | localsearch замасковано `--runtime`, без підрахунку файлів | пошук і підрахунок працюють | маска `--runtime` зникає при перезавантаженні |
| Google Drive | `rclone-gdrive.service` (VFS-кеш), стартує з kiosk | своє `exec-once rclone mount` | сервіс зупиняється при виході; автозапуску при вході немає — подвійного монтування не буде |
| Курсор | `XCURSOR_THEME` у `kioskOS/session.conf` | `~/.local/share/icons/default` mgOS не чіпається | — |
| NVIDIA | ті самі env, що й у mgOS (якщо є NVIDIA) | — | — |

Інсталятор mgOS після kiosk теж нічого не ламає: перезапис `~/.bashrc` і `~/.config/fish` повертає команду `kiosk`.
Картка «Перевірити» показує групу kioskOS (чи не лишився економ-режим, захист увімкнено, Drive не двічі).

Глобально діють лише речі, що не обмежують ПК: udev-правило тачпада (менше затримок), `nowatchdog loglevel=3`,
розширення Firefox, intel-media-driver.

Стан: `kioskos-mode status`. Повернути вручну: `kioskos-mode guard`.

## Запуск

| Як | Команда |
|---|---|
| GUI → картка «Встановити kioskOS» | `./install-gui.sh` |
| GUI одразу на kioskOS | `./install-gui.sh --kiosk` |
| Текстовий (TTY) | `./install.sh` → «kioskOS», або `./install.sh --kiosk` |
| Окремий крок вручну | `MGOS_DIR=$PWD bash gui/steps.sh kiosk_verify` |

Після встановлення в меню сесій SDDM — лише два пункти: **mgOS** і **kiosk** (`/usr/local/share/mgos-sessions`,
`/etc/sddm.conf.d/20-mgos-sessions.conf` → `[Wayland] SessionDir`). Пакетні «Hyprland» / «Hyprland (uwsm)» приховані, але не видалені.
З TTY: `kiosk`.

## Оновити знімок

На kiosk-машині після змін у kiosk.conf / waybar / runner / Firefox:

```bash
./kiosk/sync_kiosk.sh          # або ./sync_from_system.sh — він теж викличе sync_kiosk.sh
```

Знімок лежить у `kiosk/files/`. Список розширень Firefox — `kiosk/firefox-extensions.txt` (ведеться вручну).

## Кроки (gui/steps.sh, усі з точкою відкату)

| Крок | Що робить |
|---|---|
| `kiosk_pkgs` | hyprland, waybar, wofi, kitty, firefox, pipewire/wireplumber, jq, dunst, brightnessctl, playerctl, grim/slurp/wl-clipboard/hyprpicker, power-profiles-daemon, gtk4 + gtk4-layer-shell + yyjson (збірка), plocate, шрифти Cascadia/Symbols Nerd |
| `kiosk_extra` | nautilus, gvfs, btop, bluez, bluetuith, rclone, fuse3, libva-utils, firefox-i18n-uk; **intel-media-driver** на Intel; VS Code, якщо `code` немає |
| `kiosk_files` | `~/.config/hyprland` (kiosk.conf, touchpad.conf, safe-close, toggle-mode, тема Graphite, wofi, kiosk-dock, runner) + `kioskOS/session.conf` (змінні сесії), waybar kiosk + volume.py, dunst (лише якщо його ще немає), carpeted у `~/.local/bin`, `kiosk` → `kiosk-session` у bash і fish |
| `kiosk_build` | збирає Spotlight Runner і Kiosk Dock під бібліотеки цієї системи (якщо не вийшло — лишає готові бінарники) |
| `kiosk_firefox` | `kioskos.cfg` (autoconfig із user.js, діє лише при `KIOSKOS=1`); знімає глобальний kiosk user.js з профілів; розширення через `/etc/firefox/policies/policies.json` |
| `kiosk_theme` | іконки MacTahoe, курсор Bibata-Modern-Ice, шрифт SF Compact Display; Graphite і gsettings готуються в `kioskOS/session/` — вмикаються лише в kiosk |
| `kiosk_system` | udev-правило тачпада, power-profiles-daemon (профіль не змінюється), `kioskos-mode` + `kiosk-session`, `kioskos-ppd-guard.service`, `kioskos-guard.service`, rclone-gdrive.service (без автозапуску), пункти «mgOS» і «kiosk» у меню сесій |
| `kiosk_hw` | масштаб і режим екранів під **це** залізо (`~/.config/hyprland/mgos-auto.conf`, ~60 Гц), розкладки системи, тачпад без авто-сну, великий шрифт TTY на HiDPI — див. «Екран, масштаб, тачпад» |
| `kiosk_kernel` | додає `nowatchdog nvme_load=YES loglevel=3` у GRUB, якщо їх немає |
| `kiosk_verify` | перевірка команд, конфігу (`Hyprland --verify-config`), бінарників, служб, VA-API |

## Екран, масштаб, тачпад (крок `hw` у mgOS і `kiosk_hw` у kioskOS)

Знімки містять екрани машини автора (`eDP-1 … 1` у kiosk, `eDP-1 highrr ×2` і Samsung M80D ×1.5 у mgOS) —
на іншому ПК інтерфейс був би дрібним або величезним. Тепер інсталятор:

- **Масштаб за щільністю пікселів** — `scripts/mgos-display` (ставиться в `/usr/local/bin`) читає EDID
  (роздільність + фізичний розмір) і бере найбільший «чистий» масштаб Hyprland ≤ ідеального:
  ноутбук ≈121 ppi на одиницю, монітор ≈105 ppi, телевізор ≥40" — за роздільністю.
  Приклади: 15.6" FHD → 1, 14" FHD → 1.33, 14" 2.8K → 2, 16" 2560×1600 → 1.6, 27" 1440p → 1, 27" 4K → 1.6, 32" 4K → 1.33.
- **Режим**: рідна роздільність; mgOS — найвища частота, kiosk — ~60 Гц (батарея).
- Рядки `monitor =` знімка коментуються, у кінець `hyprland.conf` / `kiosk.conf` додається
  `source = …/mgos-auto.conf`. У сесії `mgos-display watch` підхоплює **нові монітори** (HDMI, док) і ставить їм масштаб сам.
- **Жести тачпада з kiosk у mgOS** (`gui/hypr-touchpad.conf` → `~/.config/hypr/touchpad.conf`): 3 пальці вбік — робочі столи
  з тією ж доводкою (smoothOut, 3.5), 3 вгору/вниз — Runner SYSui, плавний піксельний скрол, без блокування під час друку.
  Швидкість 0.25 / adaptive — лише для тачпада (`device { }`), миша mgOS лишається flat. `libinput-gestures` вимикається (конфлікт 3 пальців).
  Напрямок скролу лишається свій у кожній сесії.
- udev: тачпад без авто-сну для будь-якого I2C-HID тачпада (не лише ELAN автора); розкладки системи (`localectl`, локалі)
  додаються до `us,ua`; NumLock на ПК без батареї; `ter-132b` у TTY, якщо екран ноутбука ×2 і більше.

Що вибрано: `mgos-display detect`. Свій масштаб — `~/.config/mgos/display.conf` (діє і для автопідхоплення):

```
eDP-1     1.5
Samsung   1     2560x1440@144     # частина опису монітора, масштаб, режим (необов'язково)
```

Потім `mgos-display apply` (або перевстановити крок). «Перевірити» показує, чи масштаб відповідає екрану.

## Важливо знати

- Копіюється **фактичний** стан kiosk.conf: `vfr = false`, `direct_scanout = 0`, програмний курсор без автоприховування —
  так налаштовано після виправлення затримок тачпада (у `kioskOS/system_analysis.txt` описано старіші значення).
- auto-cpufreq зі звіту на kiosk-машині не встановлений: насправді працює power-profiles-daemon — його й ставимо.
- На kiosk-машині автора не було `intel-media-driver`, тож VA-API у Firefox не працював (vainfo падав).
  Інсталятор ставить драйвер на Intel-графіці.
- Google Drive у kiosk монтується, лише якщо в rclone є remote `drive:` (`rclone config`) і Drive ще не змонтовано.
- На kiosk-машині автора `kiosk` був alias на `Hyprland -c …` (без перемикача) — інсталятор замінює його на `kiosk-session`.
- Розширення Firefox (policies.json) діють в обох сесіях; The Tab Suspender можна вимкнути в about:addons, видалити — ні.
- Точка відкату в інсталяторі одна: після встановлення kioskOS скасувати попереднє встановлення mgOS кнопкою вже не можна.
