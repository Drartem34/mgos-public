-- ╔══════════════════════════════════════════════════════════════╗
-- ║                 HYPRLAND CONFIG — mgOS (Lua)                 ║
-- ║      Hyprland ≥ 0.57: старий hyprland.conf більше не читається ║
-- ╚══════════════════════════════════════════════════════════════╝
-- Старий конфіг (для порівняння): hyprland.conf — Hyprland 0.57 його вже не читає
-- Файли, які пишуть інші програми, лишаються у старому форматі й читаються через mgos/legacy.lua:
--   ~/.cache/wal/colors-hyprland.conf  (pywal)        blur-layers.conf (mgos-bakeblur)
--   blur-user.conf, mgos-close.conf, touchpad-user.conf (Налаштування)   mgos-auto.conf (інсталятор)

local HOME   = os.getenv("HOME")
local HYPR   = HOME .. "/.config/hypr"
local SYSUI  = HOME .. "/.config/SYSui"
local legacy = dofile(HYPR .. "/mgos/legacy.lua")
local D      = hl.dsp

local function sock(name, msg)   -- echo msg | socat - UNIX-CLIENT:~/.config/SYSui/<name>.sock
    return D.exec_cmd("echo " .. (msg or "toggle") .. " | socat - UNIX-CLIENT:" .. SYSUI .. "/" .. name .. ".sock")
end
local function ignis_action(a)
    return D.exec_cmd("gdbus call --session --dest com.github.linkfrg.ignis --object-path /com/github/linkfrg/ignis"
        .. " --method org.gtk.Actions.Activate \"" .. a .. "\" [] {}")
end


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  КОЛЬОРИ (pywal) І ЗМІННІ
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
legacy.source(HOME .. "/.cache/wal/colors-hyprland.conf")   -- $color0..$color15
local C = setmetatable({}, { __index = function(_, k) return legacy.vars[k] or "rgb(888888)" end })

local cursorTheme   = "Bibata-Modern-Classic"
local cursorSize    = "16"
local windowRounding = 15

local notifycmd      = "dunstify"
local term           = "wezterm"
local filemanagertui = term .. " -e fish -c yy"
local browser        = "firefox"


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  MONITORS — у mgos-auto.conf (інсталятор, внизу файлу)
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  ENV
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
hl.env("TERM", "wezterm")
hl.env("EDITOR", "micro")
hl.env("LIBSEAT_BACKEND", "logind")

-- NVIDIA — лише якщо драйвер nvidia завантажено (на Intel/AMD ці змінні ламають VA-API у Firefox).
-- Раніше це робив інсталятор sed-ом по hyprland.conf; тепер конфіг вирішує сам.
local function exists(p) local f = io.open(p, "r"); if f then f:close(); return true end return false end
if exists("/proc/driver/nvidia/version") then
    hl.env("LIBVA_DRIVER_NAME", "nvidia")
    hl.env("GBM_BACKEND", "nvidia-drm")
    -- hl.env("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
    hl.env("WLR_NO_HARDWARE_CURSORS", "1")
    hl.env("NVD_BACKEND", "direct")
end

-- Wayland
hl.env("XDG_CURRENT_DESKTOP", "Hyprland")
hl.env("XDG_SESSION_TYPE", "wayland")
hl.env("XDG_SESSION_DESKTOP", "Hyprland")
hl.env("USE_WAYLAND_GRIM", "1")
hl.env("GDK_BACKEND", "wayland,x11")
hl.env("SDL_VIDEODRIVER", "wayland,x11,windows")
hl.env("CLUTTER_BACKEND", "wayland")
hl.env("ELECTRON_OZONE_PLATFORM_HINT", "auto")
hl.env("MOZ_ENABLE_WAYLAND", "1")

-- Qt
hl.env("QT_QPA_PLATFORM", "wayland;xcb")
hl.env("QT_QPA_PLATFORMTHEME", "qt6ct")
hl.env("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1")
hl.env("QT_AUTO_SCREEN_SCALE_FACTOR", "1")

hl.env("_JAVA_AWT_WM_NONREPARENTING", "1")

-- GTK ≥ 4.24 сам керує блюром через протокол ext-background-effect-v1 і вимикає його для всіх вікон,
-- якщо CSS програми не просить backdrop-filter — тоді layerrule/blur Hyprland не діє (SYSui, ignis, Nautilus).
-- Вимикаємо протокол для GTK — блюр знову веде Hyprland, як до оновлення.
hl.env("GDK_WAYLAND_DISABLE", "ext_background_effect_manager_v1", true)

-- Cursor
hl.env("XCURSOR_THEME", cursorTheme)
hl.env("XCURSOR_SIZE", cursorSize)
hl.env("HYPRCURSOR_THEME", cursorTheme)
hl.env("HYPRCURSOR_SIZE", cursorSize)

-- (було: exec = gsettings set $GNOME_SCHEMA font-name … — змінна ніде не задана, команда ніколи не спрацьовувала)


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  INPUT / GENERAL / DECORATION / MISC
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
hl.config({
    input = {
        kb_layout          = "us,ua",
        kb_options         = "grp:alt_shift_toggle",
        numlock_by_default = false,
        repeat_delay       = 250,
        repeat_rate        = 35,
        focus_on_close     = 1,
        follow_mouse       = 1,
        accel_profile      = "flat",
        sensitivity        = 0.3,
        touchpad = {
            natural_scroll       = false,
            disable_while_typing = true,
            scroll_factor        = 0.8,
        },
    },

    binds  = { scroll_event_delay = 0 },
    cursor = { hotspot_padding = 0 },   -- 0: курсор доходить до самого краю — Firefox (F11) показує вкладки при наведенні вгору
    xwayland = { force_zero_scaling = true },

    general = {
        layout          = "dwindle",
        allow_tearing   = false,   -- вертикальна синхронізація
        gaps_workspaces = 0,
        gaps_in         = 3,
        gaps_out        = { top = 5, right = 10, bottom = 10, left = 10 },
        border_size     = 3,
        col = {
            active_border   = { colors = { C.color1, C.color6 }, angle = 45 },   -- динамічні кольори з pywal
            inactive_border = "rgba(595959aa)",
        },
    },

    dwindle = {
        preserve_split       = true,
        smart_split          = false,
        smart_resizing       = true,
        special_scale_factor = 0.9,
    },

    decoration = {
        rounding = windowRounding,
        blur = {
            enabled           = true,
            xray              = false,
            special           = true,
            ignore_opacity    = true,
            new_optimizations = true,
            popups            = true,
            input_methods     = true,
            size              = 5,
            passes            = 4,
        },
        shadow = {
            enabled      = true,
            range        = 10,
            render_power = 4,   -- максимум 4 (старий конфіг мав 5 — hyprlang мовчки обрізав)
            color        = "rgba(0b1215e8)",
        },
        dim_inactive = true,
        dim_strength = 0.2,
        dim_special  = 0.5,
    },

    misc = {
        vrr                          = 0,
        animate_manual_resizes       = false,
        animate_mouse_windowdragging = false,
        disable_hyprland_logo        = true,
        force_default_wallpaper      = 0,
        allow_session_lock_restore   = true,
        middle_click_paste           = false,
        focus_on_activate            = true,
        session_lock_xray            = true,
        mouse_move_enables_dpms      = true,
        key_press_enables_dpms       = true,
        background_color             = "rgb(000000)",   -- як у SDDM при виході — без синього спалаху до появи шпалер
    },

    debug = {
        vfr            = true,
        error_position = 1,
    },

    group = {
        col = {
            border_active          = { colors = { C.color4, C.color6 }, angle = 45 },
            border_inactive        = C.color0,
            border_locked_active   = C.color4,
            border_locked_inactive = C.color0,
        },
        groupbar = {
            font_family               = "JetBrains Mono NF",
            font_size                 = 15,
            gradients                 = true,
            gradient_round_only_edges = false,
            gradient_rounding         = 5,
            height                    = 25,
            indicator_height          = 0,
            gaps_in                   = 3,
            gaps_out                  = 3,
            text_color                = C.color15,
            col = {
                active          = C.color2,
                inactive        = C.color0,
                locked_active   = C.color2,
                locked_inactive = C.color0,
            },
        },
    },
})


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  ANIMATIONS (було animations.hl)
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
hl.config({ animations = { enabled = true } })

local function bezier(name, x1, y1, x2, y2)
    hl.curve(name, { type = "bezier", points = { { x1, y1 }, { x2, y2 } } })
end
bezier("linear",            0.0,  0.0,  1.0,  1.0)
bezier("specialWorkSwitch", 0.05, 0.7,  0.1,  1)
bezier("emphasizedAccel",   0.3,  0,    0.8,  0.15)
bezier("emphasizedDecel",   0.05, 0.7,  0.1,  1)
bezier("standard",          0.2,  0,    0,    1)
bezier("mgosSpring",        0.16, 1.18, 0.3,  1)   -- пружина з легким перельотом (вхід у mgOS, OSD)
bezier("mgosReveal",        0.25, 0.1,  0.25, 1)   -- м'яке проявлення шпалер

local function anim(leaf, speed, curve, style)
    hl.animation({ leaf = leaf, enabled = true, speed = speed, bezier = curve, style = style })
end
anim("borderangle",      50, "linear", "loop")   -- рамка: лише один рядок (mgos-power міняє його на батареї)
anim("layersIn",          6, "mgosSpring", "slide")
anim("layersOut",         4, "emphasizedAccel", "slide")
anim("fadeLayersIn",      9, "mgosReveal")
anim("fadeLayersOut",     4, "standard")
anim("windowsIn",         5, "emphasizedDecel")
anim("windowsOut",        3, "emphasizedAccel")
anim("windowsMove",       6, "standard")
anim("workspaces",        5, "standard", "slidefade")
anim("specialWorkspace",  4, "specialWorkSwitch", "slidefadevert 15%")
anim("fade",              6, "standard")
anim("fadeDim",           6, "standard")
anim("fadeSwitch",        2, "emphasizedAccel")   -- Super+Q → сховище: вікно швидко гасне перед переносом (mgos-close)
anim("border",            6, "standard")


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  LAYER RULES
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
local function layer(ns, t) t.match = { namespace = ns }; hl.layer_rule(t) end

-- Вхід у mgOS: шпалери проявляються з темряви → топбар падає згори → док піднімається знизу
layer("wallpaper",    { no_anim = true })            -- постер шпалер (swaybg) — з'являється миттєво
layer("mpvpaper",     { animation = "fade" })
layer("awww-daemon",  { animation = "fade" })
layer("topbar",       { animation = "slide top" })
layer("dock",         { animation = "slide bottom" })
layer("dock_cards",   { no_anim = true })             -- Простір (Super+A): картки док анімує сам

layer("waybar",           { blur = true })
layer("ignis_wallpapers", { blur = true, ignore_alpha = 0 })
layer("ignis_dock",       { blur = true, ignore_alpha = 0 })
layer("ignis_powermenu",  { blur = true, ignore_alpha = 0 })
layer("ignis_about",      { blur = true, ignore_alpha = 0 })
layer("ignis_switcher",   { blur = true, ignore_alpha = 0 })
layer("ignis_launcher",   { blur = true, ignore_alpha = 0 })
layer("ignis",            { blur = true, ignore_alpha = 0.5 })
layer("ignis_clipboard",  { blur = true, ignore_alpha = 0 })
layer("ignis_bar",        { blur = true, ignore_alpha = 0.5 })
layer("mgos-dashboard",   { no_anim = true })   -- дашборд: без fade при зміні столу

-- Живий блюр віджетів SYSui — у blur-layers.conf; його веде mgos-bakeblur
legacy.source(HYPR .. "/blur-layers.conf")
layer("mgos-genie",     { no_anim = true })   -- «джин»: 3 пальці вниз → вікно в док, кадри йдуть за пальцями
layer("clock_popover",  { no_anim = true })   -- годинник-блок: морф малюємо самі
layer("wallpapers",     { blur = true, ignore_alpha = 0.5 })


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  AUTOSTART
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
local startup = {
    -- System / Env
    "systemctl --user import-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP",
    "dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP",
    "/usr/local/bin/cleanup-cache.sh",
    "~/.local/bin/mgos-power watch",   -- режим батареї
    "rclone mount drive: ~/GoogleDrive --vfs-cache-mode full --allow-non-empty --dir-cache-time 1000h --poll-interval 5m --vfs-fast-fingerprint --buffer-size 16M",

    -- UI & Daemons
    "~/.config/SYSui/wallpaper_restore.sh",   -- постер (swaybg) → живі шпалери поверх
    "systemctl --user start dunst.service",
    "ignis init",
    "wl-paste --type text --watch cliphist store",
    "wl-paste --type image --watch cliphist store",

    -- UI & C++
    "~/.local/bin/mgos-brightness restore",
    SYSUI .. "/osd_hub",                       -- OSD гучності, яскравості, сповіщень і музики
    "sleep 0.55 && " .. SYSUI .. "/dock",      -- док виїжджає після топбару
    SYSUI .. "/powermenu_osd",
    SYSUI .. "/clipboard_osd",
    "sleep 0.3 && " .. SYSUI .. "/topbar",     -- топбар після проявлення шпалер
    "~/.local/bin/mgos-share daemon",          -- Mg Share
    SYSUI .. "/runner_osd",
    SYSUI .. "/controller_addon",
    "bash " .. SYSUI .. "/wallpapers_watch.sh",
    SYSUI .. "/cc_osd",
    SYSUI .. "/about_osd",
    "sleep 1 && " .. SYSUI .. "/dashboard",    -- дашборд робочого столу 1
    "sleep 0.9 && ~/.local/bin/mgos-sound desktop-login",
    "sleep 3 && ~/.local/bin/mgos-fan apply",

    -- Dunst & Shell Scripts
    "~/.config/dunst/volume_notify.sh",
    "~/.config/dunst/batterynotify",
    "~/.config/dunst/batteryhibernate",

    -- Apps
    "spotify",
    "vesktop --start-minimized",
}

-- Чи є на цьому ПК програма й шляхи з команди (як робив інсталятор для exec-once у hyprland.conf):
-- «sleep 1 && ~/x», «bash ~/y.sh» — перевіряємо саму програму; шляхи /… і ~/… у команді мають існувати.
local PATHS = {}
for d in (os.getenv("PATH") or "/usr/bin"):gmatch("[^:]+") do PATHS[#PATHS + 1] = d end
local function expand(p) return (p:gsub("^~", HOME)) end
local function has_prog(p)
    if p:find("/", 1, true) then return exists(expand(p)) end
    for _, d in ipairs(PATHS) do if exists(d .. "/" .. p) then return true end end
    return false
end
local function available(cmd)
    local c = cmd:gsub("^%s*sleep%s+[%d%.]+%s*&&%s*", "")
    local prog = c:match("^(%S+)")
    if not prog then return false end
    if prog == "bash" or prog == "sh" or prog == "python3" then
        local arg = c:match("^%S+%s+([~/][^%s\"';&|]+)")
        if arg and not exists(expand(arg)) then return false end
    elseif not has_prog(prog) then
        return false
    end
    for p in c:gmatch("[%s\"]([~/][^%s\"';&|]+)") do
        if not p:match("^/dev/") and not p:match("^/tmp/") and not exists(expand(p)) then return false end
    end
    return true
end

local function autostart()
    local skipped = {}
    local function run(cmd, rules)
        if available(cmd) then hl.exec_cmd(cmd, rules) else skipped[#skipped + 1] = cmd end
    end
    for _, cmd in ipairs(startup) do run(cmd) end
    -- QwikShell Web Server — тихо на 9-й стіл
    run('kitty --class kitty-service -e sh -c "sleep 2 && cd /mnt/newdisk/SLOWSHELL && python3 start.py"',
        { workspace = "9 silent" })
    for _, cmd in ipairs(legacy.startup) do run(cmd) end   -- exec-once з mgos-auto.conf тощо
    if #skipped > 0 then   -- для діагностики: що не запустилось, бо програми/шляху нема
        local f = io.open(HOME .. "/.cache/mgos-hypr-autostart-skipped.log", "w")
        if f then f:write(table.concat(skipped, "\n"), "\n"); f:close() end
    end
end
_G.mgos_autostart = autostart   -- ручний запуск: hyprctl eval 'mgos_autostart()'
_G.mgos_autostart_list = function()   -- що запуститься / що пропуститься (без запуску)
    local out = {}
    for _, cmd in ipairs(startup) do out[#out + 1] = (available(cmd) and "+ " or "- ") .. cmd end
    for _, cmd in ipairs(legacy.startup) do out[#out + 1] = (available(cmd) and "+ " or "- ") .. cmd end
    return out
end
hl.on("hyprland.start", autostart)


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  KEYBINDS
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
local bind = hl.bind
local exec = D.exec_cmd
local R    = { repeating = true }
local L    = { locked = true }
local LR   = { locked = true, repeating = true }

-- ── Core / System ──────────────────────────────────────────────
legacy.source(HYPR .. "/mgos-close.conf")   -- Super+Q: закрити або сховати в сховище доку (Налаштування → Док)
bind("SUPER + P", D.exit())
bind("SUPER + F", D.window.fullscreen({ mode = "fullscreen" }))
bind("SUPER + ALT + CTRL + F12", exec(HYPR .. "/resurrect.sh"))
bind("SUPER + Y", exec('mpv "$(wl-paste)"'))
bind("SUPER + S", D.layout("togglesplit"))
bind("SUPER + Space", function()
    hl.dispatch(D.window.float({ action = "toggle" }))
    hl.dispatch(D.window.resize({ x = 1000, y = 600, relative = false }))
end)
bind("SUPER + SHIFT + F1", exec(HYPR .. "/scripts/toggle-power-save.sh"))
bind("SUPER + A", D.workspace.toggle_special("mgos-dock"))   -- Сховище доку: сховані вікна + ярлики

bind("SUPER + O",   sock("powermenu", "show"))
bind("SUPER + V",   sock("clipboard", "toggle"))
bind("SUPER + Z",   sock("cc", "toggle"))
bind("ALT + Space", sock("runner", "show"))

bind("SUPER + F4",         exec(HYPR .. "/scripts/toggle-mode.sh"))   -- економ → баланс → продуктивний
bind("SUPER + SHIFT + F4", exec("kitty -e python " .. HOME .. "/QSH/SLOWSHELL/start.py"))

-- ── Apps ───────────────────────────────────────────────────────
bind("SUPER + B",        exec(browser))
bind("SUPER + K",        exec(term))
bind("SUPER + T",        exec("kitty --hold -e fastfetch"))
bind("CTRL + SUPER + C", exec("kitty --class claude-chat --directory " .. HOME .. " -e " .. HOME .. "/.npm-global/bin/claude --model claude-opus-5-5"))
bind("SUPER + Tab",      exec("nautilus --new-window"))   -- нове вікно на поточному столі
bind("SUPER + E",        exec("pkill -USR1 -x topbar"))   -- розширений годинник (календар/музика/таймер)
bind("SUPER + SHIFT + E", exec(filemanagertui))
bind("CTRL + SHIFT + Escape", exec("kitty -e btop"))
bind("CTRL + ALT + Space", exec("wofi"))

-- ── Ignis / Custom UI ──────────────────────────────────────────
bind("SUPER + D", exec("dunstctl close-all"))
bind("SUPER + I", sock("about", "toggle"))
bind("SUPER + U", exec("python3 ~/.config/ignis/settings.py"))
bind("SUPER + W", sock("wallpapers", "toggle"))
bind("ALT + W",   sock("wallpapers", "toggle"))
-- Ignis Window Switcher
bind("ALT + Tab",         ignis_action("switcher-next"))
bind("ALT + SHIFT + Tab", ignis_action("switcher-prev"))
bind("ALT + Alt_L",       ignis_action("switcher-release"), { release = true, transparent = true })

-- ── Screenshots & Utils ────────────────────────────────────────
bind("SUPER + SHIFT + s", exec([[sh -c 'hyprpicker -r -z & pid=$!; sleep 0.2; grim -g "$(slurp -d)" - | wl-copy && ~/.local/bin/mgos-sound screen-capture; kill $pid || true']]))
bind("ALT + F12",         exec("grim ~/Pictures/screenshots/$(date +%Y-%m-%d_%H-%M-%S).png && ~/.local/bin/mgos-sound screen-capture"))
bind("SHIFT + Print",     exec("grim - | wl-copy -t image/png && ~/.local/bin/mgos-sound screen-capture"))
bind("SUPER + SHIFT + X", exec("hyprpicker -a"))

-- ── Waybar ─────────────────────────────────────────────────────
bind("SUPER + SHIFT + W", exec("killall waybar; waybar"))

-- ── Media & Volume ─────────────────────────────────────────────
local media = HYPR .. "/scripts/media.sh"
local volsock = "echo show | socat - UNIX-CLIENT:" .. SYSUI .. "/volume.sock"
bind("SUPER + F10", exec(media .. " play-pause && ~/.config/dunst/music_notify.sh"))
bind("SHIFT + F8",  exec(media .. " next"))
bind("SHIFT + F7",  exec(media .. " previous"))
bind("SHIFT + F9",  exec(media .. " play-pause"))

bind("SUPER + F7",         exec("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- && " .. volsock), R)
bind("SUPER + F8",         exec("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+ && " .. volsock), R)
bind("SUPER + SHIFT + F7", exec("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle && " .. volsock))

bind("XF86AudioPlay", exec(media .. " play-pause"), L)
bind("XF86AudioNext", exec(media .. " next"), L)
bind("XF86AudioPrev", exec(media .. " previous"), L)
bind("XF86AudioRaiseVolume", exec("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+ && " .. volsock .. "; ~/.local/bin/mgos-sound audio-volume-change"), L)
bind("XF86AudioLowerVolume", exec("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- && " .. volsock .. "; ~/.local/bin/mgos-sound audio-volume-change"), L)

-- ── Brightness (плашка SYSui brightness_osd; працює й на екрані блокування) ──
bind("XF86MonBrightnessUp",   sock("brightness", "up"),   LR)
bind("XF86MonBrightnessDown", sock("brightness", "down"), LR)
bind("SUPER + F3",            sock("brightness", "up"),   LR)   -- для клавіатур без клавіш яскравості
bind("SUPER + F2",            sock("brightness", "down"), LR)

-- ── Window Focus & Movement ────────────────────────────────────
for key, dir in pairs({ left = "left", right = "right", up = "up", down = "down" }) do
    bind("SUPER + " .. key,         D.focus({ direction = dir }))
    bind("SUPER + SHIFT + " .. key, D.window.move({ direction = dir }), R)
end
bind("SUPER + CTRL + left",  D.window.resize({ x = -40, y = 0,   relative = true }), R)
bind("SUPER + CTRL + right", D.window.resize({ x = 40,  y = 0,   relative = true }), R)
bind("SUPER + CTRL + up",    D.window.resize({ x = 0,   y = -40, relative = true }), R)
bind("SUPER + CTRL + down",  D.window.resize({ x = 0,   y = 40,  relative = true }), R)

-- ── Groups ─────────────────────────────────────────────────────
bind("SUPER + G", D.group.toggle())
bind("SUPER + C", D.window.move({ out_of_group = true }))
bind("SUPER + X", D.group.next())

-- ── Workspaces ─────────────────────────────────────────────────
-- (SUPER+SHIFT+Space «workspaceopt allfloat» — у Hyprland 0.57 такого диспетчера немає)
for i = 1, 10 do
    local key = tostring(i % 10)   -- 10 → клавіша 0
    bind("SUPER + " .. key,         D.focus({ workspace = i }))
    bind("SUPER + SHIFT + " .. key, D.window.move({ workspace = i, follow = false }))   -- тихо
end
bind("SUPER + ALT + up",   D.focus({ workspace = "m+1" }))
bind("SUPER + ALT + down", D.focus({ workspace = "m-1" }))

-- ── Mouse Binds ────────────────────────────────────────────────
bind("SUPER + mouse:272", D.window.drag(),   { mouse = true })
bind("SUPER + mouse:273", D.window.resize(), { mouse = true })
bind("SUPER + mouse:274", D.window.close())


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  WINDOW RULES
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
local function rule(match, t) t.match = match; hl.window_rule(t) end

-- Floating & Centering
rule({ class = "^(org.kde.polkit-kde-authentication-agent-1)$" }, { float = true })
rule({ title = "^(Media viewer)$" },     { float = true })
rule({ title = "^(Volume Control)$" },   { float = true, move = "(75) ((monitor_h*0.44))" })
rule({ class = "^(dialog)$" },           { float = true })
rule({ class = "^(com.megumin.installer)$" }, { float = true, center = true })
rule({ class = "^(megumin-settings)$" }, { float = true, size = "980 660", center = true })
rule({ class = "^(download)$" },         { float = true })
rule({ class = "^(notification)$" },     { float = true })
rule({ class = "^(error)$" },            { float = true })
rule({ title = "^(Open File)$" },        { float = true })
rule({ class = "(dolphin)" },            { float = true })
rule({ class = "(kate)" },               { float = true })
rule({ class = "^(wofi)$" },             { float = true, no_focus = true })

rule({ float = true }, { center = true })

-- Workspaces Binding
rule({ class = "^(vesktop)$" },              { workspace = "2" })
rule({ class = "^(org.telegram.desktop)$" }, { workspace = "2" })
rule({ class = "^(obsidian)$" },             { workspace = "2" })
rule({ class = "^(Spotify)$" },              { workspace = "1" })
rule({ class = "(idea)" },                   { workspace = "4" })
rule({ class = "^(Code)$" },                 { workspace = "4" })
rule({ class = "^(obs)$" },                  { workspace = "5" })
rule({ class = "(keepassxc)" },              { workspace = "10" })

-- Specific Hacks
rule({ class = "^(.*jetbrains.*)$", title = "^(win.*)$" }, { no_initial_focus = true })
rule({ class = "^(it.catboy.ripdrag)$" }, {
    pin  = true,
    move = "(cursor_x+(min(max(-(monitor_w*0.05),0),monitor_w-window_w))) (cursor_y+(min(max((monitor_h*0),0),monitor_h-window_h)))",
})

-- (plugin hyprwinwrap { class = mpv } — плагін не встановлено, пропущено)


-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
--  ФАЙЛИ «НАЛАШТУВАНЬ» ТА ІНСТАЛЯТОРА (старий формат)
-- ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
legacy.source(HYPR .. "/blur-user.conf")   -- розмиття з Налаштувань

-- Налаштування → Клавіатура → «Перевірка клавіш»: на час тесту гарячі клавіші mgOS вимкнені;
-- аварійний вихід — Super+Ctrl+Shift+Esc.
hl.define_submap("mgos-keytest", function()
    bind("SUPER + CTRL + SHIFT + Escape", D.submap("reset"))
end)

legacy.source(HYPR .. "/mgos-auto.conf")   -- автоналаштування під це залізо (екрани, NumLock, тачпад)


-- ━━ помилки розбору старих файлів — одним сповіщенням ━━
if #legacy.errors > 0 then
    local f = io.open(HOME .. "/.cache/mgos-hypr-legacy.log", "w")
    if f then f:write(table.concat(legacy.errors, "\n"), "\n"); f:close() end
    hl.notification.create({
        text = "mgOS: " .. #legacy.errors .. " помилок у старих .conf — ~/.cache/mgos-hypr-legacy.log",
        timeout = 8000,
    })
end
