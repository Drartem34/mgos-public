# mgOS — Megumin OS 6 installer

Інсталятор Megumin OS 6 (Hyprland + панелі SYSui) і енергоощадної сесії kioskOS для Arch Linux.

## Найпростіше: з чистого Arch (TTY)

```bash
sudo pacman -Sy --needed github-cli && gh auth login
gh api repos/Drartem34/mgos-public/contents/mgos-bootstrap.sh --jq .content | base64 -d > mgos-bootstrap.sh
bash mgos-bootstrap.sh
```

`mgos-bootstrap.sh` ставить мінімум (git, gh, Hyprland, GTK4), входить у GitHub, клонує інсталятор у `~/mgOS`
і запускає **mgOS Installer на весь екран** у мінімальній сесії Hyprland. Після встановлення — «Перезавантажити»,
вхід у mgOS, і з'являється вікно **«Все готово до роботи»** (один раз).
Аварійно: `Super+Enter` — термінал, `Ctrl+Alt+Del` — вихід у TTY.

## Сповіщення про нову версію

У сесії mgOS і kiosk `gui/update-notify.sh` раз на 6 год (перший раз через 3 хв після входу) перевіряє GitHub.
Є нові коміти → сповіщення «Є нова версія mgOS» з кнопкою «Відкрити інсталятор». Про ту саму версію — лише раз.
Вимкнути: `touch ~/.config/mgos/no-update-notify`.

## Встановлення вручну

```bash
sudo pacman -S --needed git github-cli
gh auth login                      # репозиторій приватний
gh repo clone mgos ~/mgOS          # або: git clone https://github.com/<user>/mgos ~/mgOS
~/mgOS/install-gui.sh              # графічний; з TTY — ~/mgOS/install.sh
```

Після першого запуску в списку програм з'являється **mgOS Installer** (дії: kioskOS, текстовий режим).

## Оновлення інсталятора

- **GUI**: на головній сторінці з'являється банер «Нова версія на GitHub» → «Оновити інсталятор»
  (git pull, перезбірка, перезапуск).
- **TUI**: `install.sh` при старті сам питає, чи оновитись.
- Вручну: `gui/selfupdate.sh check` / `gui/selfupdate.sh apply`.

Локальні правки не губляться (`git pull --ff-only --autostash`).

## Що НЕ в репозиторії

- `wallpapers/` (кілька ГБ): поклади теку поруч вручну, якщо потрібні шпалери. Без неї крок шпалер просто пропускається.
- `configs/rclone/rclone.conf` (токени Google Drive): після встановлення виконай `rclone config`.
- `gui/mgos-installer`: збирається на кожній машині (`install-gui.sh` робить це сам).

## Оновити знімок з цієї системи

```bash
./sync_from_system.sh   # mgOS (і kiosk, якщо це kiosk-машина)
git add -A && git commit -m "Знімок $(date +%F)" && git push
```

Докладно про kioskOS, масштаб екрана й жести тачпада: [kiosk/README.md](kiosk/README.md).

---
**Публічна версія.** Без шрифтів Apple SF (замість них — вільний Inter, вигляд дуже схожий; якщо SF Pro вже є
в `~/.local/share/fonts`, використовується він) і без особистих налаштувань автора.
Встановлення без входу в GitHub:

```bash
curl -fsSLO https://raw.githubusercontent.com/Drartem34/mgos-public/main/mgos-bootstrap.sh
bash mgos-bootstrap.sh
```
