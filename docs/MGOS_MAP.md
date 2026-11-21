# mgOS — карта для Claude (щоб не вивчати систему наново)

Живі файли — у `~/.config/...`; репозиторій-інсталятор — `~/.local/share/mgos/recovery` (копія через
`./sync_from_system.sh [--no-wallpapers]`, потім commit + push). Kiosk (`kiosk/`) не чіпати.

## Де що лежить
| Що | Живий шлях | Примітки |
|---|---|---|
| Панелі SYSui (C++/GTK4) | `~/.config/SYSui/*.cpp` | збірка: `./build.sh <модуль> [--restart]`; модулі — мапа `MAP` у `build.sh` |
| Налаштування (PyQt) | `~/.config/ignis/settings.py` | сторінки — `create_<id>_page`; список у `pages`/`dispatch` ~рядок 3317 |
| Ignis (старі віджети) | `~/.config/ignis/*.py` | `wallpapers.py` — селектор шпалер Ignis |
| Налаштування користувача | `~/.config/megumin-os/settings.json` (+ `defaults.json`) | у C++ — `sysui::settings_set/get` (`sysui_common.h`) |
| Поточні шпалери | `~/.cache/ignis/last_wallpaper.txt` | файл/тека або `pixel:<style>` |
| Файли шпалер | `~/Pictures/wallpapers` | тека з `day.*`/`night.*` = динамічні; у репо — `wallpapers/` |
| Привітання (перший вхід) | `~/.config/SYSui/welcome.cpp` | має свій список піксельних стилів `OPTS[]` |

## Шпалери
- **Картинки/відео**: просто покласти файл у `~/Pictures/wallpapers` — з'являться в Налаштуваннях → Шпалери,
  у SYSui (`wallpapers.cpp`, Super+W) і в Ignis. Відео — mpvpaper, картинки — awww. Відновлення при вході — `wallpaper_restore.sh`.
- **Живі піксельні** (`pixel_wallpaper`): рендерер — `pixel_waves.h`, структура `pw::Waves`.
  Новий стиль = (1) id в `enum S_*` і в масиві `N[]` у `style_id()` (оновити кількість у циклі);
  (2) `<style>_prepare()` у `render()` і гілка `if (sid == S_X) { row[x] = ...; continue; }` у циклі пікселів;
  (3) додати `("id", "Назва")` у `PIXEL_STYLES_WP` (settings.py; `PIXEL_STYLES` — це ще й стилі «шапок» панелей)
  та в `OPTS[]` у `welcome.cpp`; (4) `./build.sh pixel_wallpaper welcome`.
  Прев'ю без зміни шпалер: `pixel_wallpaper --poster out.png 1920x1080 --style ID --color '#hex' --theme dark|light`.
  Прев'ю в Налаштуваннях кешуються в `~/.cache/megumin-os/pixel_previews` (після зміни стилю видалити кеш).
  Застосувати: `pixel_wallpaper --set ID`. Перевірка анімації/швидкодії: зібрати маленький bench з
  `#include "pixel_waves.h"`, рендерити `w.t` по черзі, писати PNG кадрів (норма — кілька мс на кадр).
- Стилі: waves, virus, eq, rain, life, horizon, aurora, blackhole, neoncity (неонове місто), rooftop (дах, 2026-11-15).
- У `render()` пара `if (sid == S_X) x_prepare(); else if (!x_frame.empty()) <звільнити буфери>` — нова пара
  ЗАВЖДИ окремим рядком після попередньої `else`, інакше `else` прилипне до чужого `if` і буфери звільняться
  посеред кадру (segfault, було з neoncity). Після змін — постери всіх стилів × 2 теми + прогін з перемиканням стилів.
- Сцени з деталями фіксованого розміру (neoncity, rooftop) у `--poster` рендеряться на сітці 1920 і зменшуються.
  Персонаж у rooftop — спрайт-рядки `SPR[]` у `roof_build` (символи → кольори в switch).
- Великі стилі (як neoncity): статика — раз на розмір/тему в буфер (`ct_base` + буфер глибини `ct_dep`), щокадру лише
  копія + динамічні шари, що малюються тільки поверх пікселів з глибиною ≤ потрібної. Ітерувати на копії
  `pixel_waves.h` у scratchpad з маленьким `shot.cpp` (рендер PNG + мс/кадр), перед встановленням — прогін під
  `-fsanitize=address,undefined` на кількох роздільностях і обох темах.
- Вайб конкретної гри — через палітру, архітектуру, рекламу, смог, а не логотипи/впізнавані будівлі.

## Правила (див. також memory)
- Kiosk ніколи не ламати; не стартувати SYSui у kiosk-сесії (перевіряти активну сесію Hyprland).
- Без headless-виходів Hyprland; знімки UI — у спецворкспейсі `special:mgshot`.
- Після кожної зміни: `sync_from_system.sh` → commit → push, навіть без нагадування.
