-- ━━ mgOS: сумісність зі старим синтаксисом Hyprland (hyprlang, ≤ 0.56) ━━━━━━━━━━━━━━━━━━━━━━━━━━━
-- Hyprland 0.57 читає лише Lua (hyprland.lua). Але частину файлів і досі пишуть інші програми у
-- старому форматі (Налаштування → blur-user.conf / mgos-close.conf / touchpad-user.conf,
-- mgos-bakeblur → blur-layers.conf, інсталятор → mgos-auto.conf, pywal → colors-hyprland.conf),
-- а SYSui / скрипти шлють старі команди «dispatch workspace 2», «keyword decoration:rounding 0».
--
--   legacy.source(path)        — прочитати .conf у старому форматі й застосувати
--   legacy.keyword(k, v)       — один рядок «k = v» старого конфігу (hyprctl keyword)
--   legacy.dispatch(line)      — «workspace 2» → hl.dsp.focus({ workspace = 2 })
--
-- Глобальні mgosD(line) / mgosK(k, v) — для IPC: ~/.local/bin/hyprctl і SYSui (sysui_common.h)
-- перекладають «dispatch X» → «dispatch mgosD([==[X]==])», «keyword K V» → «eval mgosK(...)».

local M = {}
local HOME = os.getenv("HOME") or ""
local D = hl.dsp

M.errors  = {}   -- помилки розбору (показуємо одним сповіщенням)
M.startup = {}   -- exec-once зі старих файлів (запускає hyprland.lua на hyprland.start)
M.vars    = {}   -- $змінні

-- ── дрібниці ─────────────────────────────────────────────────────────────────
local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end
local function expand(p)
    p = trim(p)
    if p:sub(1, 1) == "~" then p = HOME .. p:sub(2) end
    return (p:gsub("%$HOME", HOME))
end
local function err(msg) M.errors[#M.errors + 1] = msg end

-- розбиває за комами; «\,» — кома всередині значення; max — не більше max частин
local function split(s, max)
    local out, cur, i = {}, {}, 1
    while i <= #s do
        local c = s:sub(i, i)
        if c == "\\" and s:sub(i + 1, i + 1) == "," then cur[#cur + 1] = ","; i = i + 2
        elseif c == "," and (not max or #out < max - 1) then out[#out + 1] = trim(table.concat(cur)); cur = {}; i = i + 1
        else cur[#cur + 1] = c; i = i + 1 end
    end
    out[#out + 1] = trim(table.concat(cur))
    return out
end

local function first_word(s)
    s = trim(s)
    local a, b = s:match("^(%S+)%s*(.*)$")
    return a or "", b or ""
end

local function num_or_str(v)
    local n = tonumber(v)
    if n then return n end
    if v == "on" or v == "true" or v == "yes" then return true end
    if v == "off" or v == "false" or v == "no" then return false end
    return v
end

local function ws(v) v = trim(v or ""); return tonumber(v) or v end
local function opt(v) v = trim(v or ""); if v == "" then return nil end return v end

-- підставляє $змінні (довші імена першими, щоб $color1 не з'їв $color15)
function M.subst(s)
    return (s:gsub("%$([%w_]+)", function(name)
        local v = M.vars[name]
        return v ~= nil and v or nil
    end))
end

-- «SUPER SHIFT», «SUPER_SHIFT», «SUPER+SHIFT» + «Q» → «SUPER + SHIFT + Q»
local MOD_ALIAS = { CONTROL = "CTRL", CRL = "CTRL", CTL = "CTRL", WIN = "SUPER", MOD4 = "SUPER", MOD1 = "ALT" }
function M.keys(mods, key)
    local parts = {}
    for m in (mods or ""):upper():gmatch("[%w]+") do parts[#parts + 1] = MOD_ALIAS[m] or m end
    parts[#parts + 1] = trim(key)
    return table.concat(parts, " + ")
end

-- ── диспетчери: старе ім'я + рядок аргументів → HL.Dispatcher ───────────────
local DIR = { l = "left", r = "right", u = "up", d = "down", left = "left", right = "right", up = "up", down = "down" }

-- «exact 100 200,address:0x..» / «-40 0» → x, y, relative, window
local function xy_args(a)
    local main, win = a:match("^(.-),%s*(.+)$")
    if not main then main = a end
    local exact = false
    main = trim(main)
    if main:sub(1, 6) == "exact " then exact = true; main = trim(main:sub(7)) end
    local x, y = main:match("^(%S+)%s+(%S+)$")
    return { x = tonumber(x) or x, y = tonumber(y) or y, relative = not exact, window = opt(win) }
end

-- «[workspace 9 silent; float] cmd» → cmd, { workspace = "9 silent", float = true }
local function exec_rules(a)
    a = trim(a)
    if a:sub(1, 3) == "-- " then a = trim(a:sub(4)) end
    local rules
    local r, rest = a:match("^%[(.-)%]%s*(.*)$")
    if r then
        rules = {}
        for part in r:gmatch("[^;]+") do
            local k, v = first_word(part)
            if k ~= "" then rules[k] = (v == "" and true or num_or_str(v)) end
        end
        a = rest
    end
    return a, rules
end

local T = {}
T.exec            = function(a) local c, r = exec_rules(a); return D.exec_cmd(c, r) end
T.execr           = function(a) return D.exec_raw(trim(a)) end
T.killactive      = function()  return D.window.close() end
T.forcekillactive = function()  return D.window.kill() end
T.closewindow     = function(a) return D.window.close({ window = opt(a) }) end
T.killwindow      = function(a) return D.window.kill({ window = opt(a) }) end
T.workspace       = function(a) return D.focus({ workspace = ws(a) }) end
T.focusworkspaceoncurrentmonitor = T.workspace
T.movetoworkspace = function(a, silent)
    local w, win = a:match("^(.-),%s*(.+)$")
    if not w then w = a end
    local t = { workspace = ws(w), window = opt(win) }
    if silent then t.follow = false end
    return D.window.move(t)
end
T.movetoworkspacesilent  = function(a) return T.movetoworkspace(a, true) end
T.togglespecialworkspace = function(a) return D.workspace.toggle_special(opt(a)) end
T.togglefloating  = function(a) return D.window.float({ action = "toggle",  window = opt(a) }) end
T.setfloating     = function(a) return D.window.float({ action = "enable",  window = opt(a) }) end
T.settiled        = function(a) return D.window.float({ action = "disable", window = opt(a) }) end
T.fullscreen      = function(a)  -- старе: 0 = на весь екран, 1 = розгорнути
    local mode = trim(a or "")
    return D.window.fullscreen({ mode = (mode == "1") and "maximized" or "fullscreen" })
end
T.fullscreenstate = function(a)
    local i, c = trim(a):match("^(%-?%d+)%s+(%-?%d+)")
    return D.window.fullscreen_state({ internal = tonumber(i) or 0, client = tonumber(c) or 0 })
end
T.movefocus       = function(a) return D.focus({ direction = DIR[trim(a)] or trim(a) }) end
T.movewindow      = function(a) return D.window.move({ direction = DIR[trim(a)] or trim(a) }) end
T.swapwindow      = function(a) return D.window.swap({ direction = DIR[trim(a)] or trim(a) }) end
T.resizeactive    = function(a) return D.window.resize(xy_args(a)) end
T.resizewindowpixel = T.resizeactive
T.moveactive      = function(a) return D.window.move(xy_args(a)) end
T.movewindowpixel = T.moveactive
T.focuswindow     = function(a) return D.focus({ window = trim(a) }) end
T.focusmonitor    = function(a) return D.focus({ monitor = trim(a) }) end
T.focusurgentorlast  = function()  return D.focus({ urgent_or_last = true }) end
T.focuscurrentorlast = function()  return D.focus({ last = true }) end
T.setprop         = function(a)   -- «address:0x.. opacity 0.0 override»
    local win, rest = first_word(a)
    local prop, value = first_word(rest)
    return D.window.set_prop({ window = win, prop = prop, value = value })
end
T.sendshortcut    = function(a)   -- «CTRL SHIFT, V, address:0x..»
    local p = split(a, 3)
    return D.send_shortcut({ mods = p[1], key = p[2], window = opt(p[3]) })
end
T.submap          = function(a) return D.submap(trim(a)) end
T.layoutmsg       = function(a) return D.layout(trim(a)) end
T.togglesplit     = function()  return D.layout("togglesplit") end
T.togglegroup     = function()  return D.group.toggle() end
T.changegroupactive = function(a) return (trim(a or "") == "b") and D.group.prev() or D.group.next() end
T.moveoutofgroup  = function()  return D.window.move({ out_of_group = true }) end
T.lockactivegroup = function()  return D.group.lock_active() end
T.exit            = function()  return D.exit() end
T.pin             = function(a) return D.window.pin({ action = "toggle", window = opt(a) }) end
T.centerwindow    = function()  return D.window.center() end
T.pseudo          = function()  return D.window.pseudo() end
T.bringactivetotop = function() return D.window.bring_to_top() end
T.alterzorder     = function(a) local p = split(a, 2); return D.window.alter_zorder({ mode = p[1], window = opt(p[2]) }) end
T.tagwindow       = function(a) local t, w = first_word(a); return D.window.tag({ tag = t, window = opt(w) }) end
T.signal          = function(a) return D.window.signal({ signal = tonumber(trim(a)) }) end
T.pass            = function(a) return D.pass({ window = trim(a) }) end
T.global          = function(a) return D.global(trim(a)) end
T.event           = function(a) return D.event(trim(a)) end
T.dpms            = function(a) return D.dpms(opt(a)) end
T.movecursor      = function(a) local x, y = trim(a):match("^(%S+)%s+(%S+)$"); return D.cursor.move({ x = tonumber(x), y = tonumber(y) }) end
T.renameworkspace = function(a) local id, name = first_word(a); return D.workspace.rename({ workspace = ws(id), name = opt(name) }) end
T.movecurrentworkspacetomonitor = function(a) return D.workspace.move({ monitor = trim(a) }) end
T.forcerendererreload = function() return D.force_renderer_reload() end
T.cyclenext       = function()  return D.window.cycle_next() end
-- немає в 0.57: workspaceopt (allfloat / allpseudo)

function M.dsp(name, args)
    local f = T[name]
    if not f then error("mgOS: старий диспетчер «" .. tostring(name) .. "» не підтримується в Hyprland 0.57", 0) end
    local d = f(args or "")
    if d == nil then error("mgOS: не вдалося перекласти «" .. name .. " " .. (args or "") .. "»", 0) end
    return d
end

function M.dispatch(line)
    local name, args = first_word(line)
    return M.dsp(name, args)
end

-- ── окремі ключові слова старого конфігу ──────────────────────────────────────
function M.monitor(v)
    local p = split(v)
    local spec = { output = p[1] or "" }
    if p[2] == "disable" or p[2] == "disabled" then spec.disabled = true; return hl.monitor(spec) end
    spec.mode, spec.position, spec.scale = p[2], p[3], p[4]
    local i = 5
    while p[i] and p[i + 1] do spec[p[i]] = num_or_str(p[i + 1]); i = i + 2 end
    hl.monitor(spec)
end

function M.bezier(v)
    local p = split(v)
    hl.curve(p[1], { type = "bezier", points = { { tonumber(p[2]), tonumber(p[3]) }, { tonumber(p[4]), tonumber(p[5]) } } })
end

function M.animation(v)
    local p = split(v)
    local a = { leaf = p[1], enabled = (p[2] == "1" or p[2] == "on" or p[2] == "true") }
    if a.enabled then
        a.speed = tonumber(p[3])
        if p[4] and p[4] ~= "" then a.bezier = p[4] end
        if p[5] and p[5] ~= "" then a.style = p[5] end
    end
    hl.animation(a)
end

-- «blur on, ignore_alpha 0.5, match:namespace dock» → { blur = true, ignore_alpha = 0.5, match = { namespace = "dock" } }
local MATCH_BOOL = { float = true, floating = true, xwayland = true, fullscreen = true, pin = true, pinned = true, focus = true, group = true }
local function rule_spec(v)
    local spec, match = {}, {}
    for _, part in ipairs(split(v)) do
        local mk, mv = part:match("^match:(%S+)%s+(.+)$")
        if mk then
            if MATCH_BOOL[mk] then match[mk] = (mv == "1" or mv == "true" or mv == "on")
            else match[mk] = mv end
        elseif part ~= "" then
            local k, val = first_word(part)
            spec[k] = (val == "") and true or num_or_str(val)
        end
    end
    spec.match = match
    return spec
end
function M.layerrule(v)  hl.layer_rule(rule_spec(v)) end
function M.windowrule(v) hl.window_rule(rule_spec(v)) end

M.gestures = {}   -- жести, поставлені через legacy (щоб «unset» знімав лише наявні)
-- «3, down, unset» / «3, up, dispatcher, exec, cmd» / «3, horizontal, mod: SUPER, scale: 1.5, workspace»
function M.gesture(v)
    local p = split(v)
    local g = { fingers = tonumber(p[1]), direction = p[2] }
    local i = 3
    while p[i] do
        local mod = p[i]:match("^mods?:%s*(.+)$")
        local sc  = p[i]:match("^scale:%s*(.+)$")
        if mod then g.mods = mod elseif sc then g.scale = tonumber(sc) else break end
        i = i + 1
    end
    local action = p[i] or "unset"
    if action == "dispatcher" then
        local name = p[i + 1] or ""
        local args = table.concat(p, ", ", i + 2)
        local dsp = M.dsp(name, args)   -- перевіряємо одразу, а не під час жесту
        g.action = function() hl.dispatch(dsp) end
    else
        g.action = action
        if p[i + 1] and p[i + 1] ~= "" then g.workspace_name = p[i + 1] end
    end
    -- «unset» жесту, якого ще нема, — не помилка (Налаштування завжди спершу знімають жест);
    -- Hyprland 0.57 на таке пише помилку конфігу, тож знімаємо лише ті, що ставили самі
    local id = tostring(g.fingers) .. ":" .. tostring(g.direction) .. ":" .. tostring(g.mods or "")
    if action == "unset" then
        if M.gestures[id] then M.gestures[id] = nil; hl.gesture(g) end
        return
    end
    hl.gesture(g)
    M.gestures[id] = true
end

-- налаштування окремих пристроїв накопичуємо: «device[mouse]:sensitivity» і блок device { } доповнюють одне одного
M.devices = {}
function M.device(name, key, value)
    local d = M.devices[name] or { name = name }
    M.devices[name] = d
    if key then d[key:gsub("%-", "_")] = num_or_str(value) end
    local copy = {}
    for k, val in pairs(d) do copy[k] = val end
    hl.device(copy)
end

-- bind / binde / bindl / bindm / bindr / bindd ...
local submap = nil   -- { name = ..., binds = { ... } } поки читаємо «submap = name»
local MOUSE_DSP = { movewindow = function() return D.window.drag() end, resizewindow = function() return D.window.resize() end }
function M.bind(flags, v)
    local described = flags:find("d", 1, true) ~= nil
    local p = split(v, described and 5 or 4)
    local mods, key = p[1], p[2]
    local desc
    if described then desc = table.remove(p, 3) end
    local name, args = p[3] or "", p[4] or ""
    local o = {}
    for c in flags:gmatch(".") do
        if c == "l" then o.locked = true
        elseif c == "e" then o.repeating = true
        elseif c == "r" then o.release = true
        elseif c == "n" then o.non_consuming = true
        elseif c == "t" then o.transparent = true
        elseif c == "i" then o.ignore_mods = true
        elseif c == "o" then o.long_press = true
        elseif c == "c" then o.click = true
        elseif c == "g" then o.drag = true
        elseif c == "m" then o.mouse = true end
    end
    if desc then o.description = desc end
    local keys = M.keys(mods, key)
    local dsp
    if o.mouse and MOUSE_DSP[name] then dsp = MOUSE_DSP[name]() else dsp = M.dsp(name, args) end
    if next(o) == nil then o = nil end
    if submap then
        submap.binds[#submap.binds + 1] = { keys, dsp, o }
    else
        return hl.bind(keys, dsp, o)
    end
end

function M.unbind(v)
    local p = split(v, 2)
    hl.unbind(M.keys(p[1], p[2] or ""))
end

-- «decoration:blur:enabled», «true» → hl.config({ decoration = { blur = { enabled = true } } })
function M.option(key, value)
    local dev, dkey = key:match("^device%[(.-)%]:(.+)$")
    if dev then return M.device(dev, dkey, value) end
    local path = {}
    for part in key:gsub("%-", "_"):gmatch("[^:%.]+") do path[#path + 1] = part end   -- tap-to-click → tap_to_click
    if #path == 0 then return end
    -- тип беремо з поточного значення: bool / число; кольори, градієнти, відступи — рядком (Hyprland розбере сам)
    local ok, cur = pcall(hl.get_config, table.concat(path, "."))
    local v = value
    if ok and type(cur) == "boolean" then
        v = (value == "1" or value == "true" or value == "yes" or value == "on")
    elseif ok and type(cur) == "number" then
        -- «drag_lock = false»: опція стала числовою (0/1/2), а «Налаштування» пишуть true/false
        local b = ({ ["true"] = 1, ["yes"] = 1, ["on"] = 1, ["false"] = 0, ["no"] = 0, ["off"] = 0 })[value]
        v = tonumber(value) or b or value
    end
    local t = {}
    local node = t
    for i = 1, #path - 1 do node[path[i]] = {}; node = node[path[i]] end
    node[path[#path]] = v
    hl.config(t)
end

-- ── один рядок старого конфігу ───────────────────────────────────────────────
function M.keyword(k, v, ctx)
    k = trim(k); v = M.subst(trim(v or ""))
    local bindflags = k:match("^bind(%a*)$")
    if bindflags then return M.bind(bindflags, v) end
    if k == "unbind"     then return M.unbind(v) end
    if k == "monitor"    then return M.monitor(v) end
    if k == "bezier"     then return M.bezier(v) end
    if k == "animation"  then return M.animation(v) end
    if k == "layerrule"  then return M.layerrule(v) end
    if k == "windowrule" or k == "windowrulev2" then return M.windowrule(v) end
    if k == "gesture"    then return M.gesture(v) end
    if k == "env"        then local p = split(v, 2); return hl.env(p[1], p[2] or "") end
    if k == "source"     then return M.source(v) end
    if k == "exec-once"  then
        if ctx and ctx.loading then M.startup[#M.startup + 1] = v else hl.exec_cmd(exec_rules(v)) end
        return
    end
    if k == "exec"       then local c, r = exec_rules(v); return hl.exec_cmd(c, r) end
    if k == "submap"     then
        if v == "reset" then
            if submap then
                local s = submap; submap = nil
                hl.define_submap(s.name, function()
                    for _, b in ipairs(s.binds) do hl.bind(b[1], b[2], b[3]) end
                end)
            end
        else
            submap = { name = v, binds = {} }
        end
        return
    end
    return M.option(k, v)
end

-- ── файл у старому форматі ───────────────────────────────────────────────────
local function strip_comment(line)
    -- «#» — коментар; «##» — буквальний «#»
    local out, i = {}, 1
    while i <= #line do
        local c = line:sub(i, i)
        if c == "#" then
            if line:sub(i + 1, i + 1) == "#" then out[#out + 1] = "#"; i = i + 2
            else break end
        else out[#out + 1] = c; i = i + 1 end
    end
    return table.concat(out)
end

function M.source(path)
    path = expand(path)
    local f = io.open(path, "r")
    if not f then return end   -- як і раніше: нема файлу — нема налаштувань
    local stack = {}            -- секції: { "decoration", "blur" }
    local device                -- device { name = ... } збираємо цілком
    local skip = 0              -- plugin { } — плагінів немає, пропускаємо
    local n = 0
    for raw in f:lines() do
        n = n + 1
        local line = trim(strip_comment(raw))
        if line == "" or line:sub(1, 1) == ";" then goto continue end

        if skip > 0 then
            if line:find("{", 1, true) then skip = skip + 1 end
            if line:find("}", 1, true) then skip = skip - 1 end
            goto continue
        end

        do
            local sect = line:match("^([%w_%-%.:]+)%s*{$")
            if sect then
                if #stack == 0 and sect == "plugin" then skip = 1
                elseif #stack == 0 and sect == "device" then device = {}; stack[#stack + 1] = sect
                else stack[#stack + 1] = sect end
                goto continue
            end
        end

        if line == "}" then
            local closed = table.remove(stack)
            if closed == "device" and #stack == 0 and device then
                local ok, e = pcall(function()
                    local name = tostring(device.name or "")
                    for k, val in pairs(device) do
                        if k ~= "name" then M.device(name, k, tostring(val)) end
                    end
                    if next(device, nil) == "name" and next(device, "name") == nil then M.device(name) end
                end)
                if not ok then err(path .. ":" .. n .. ": device: " .. tostring(e)) end
                device = nil
            end
            goto continue
        end

        do
            local k, v = line:match("^([^=]-)%s*=%s*(.*)$")
            if not k then err(path .. ":" .. n .. ": не розумію рядок «" .. line .. "»"); goto continue end
            k = trim(k)
            if k:sub(1, 1) == "$" then M.vars[k:sub(2)] = M.subst(trim(v)); goto continue end
            if device then device[k] = num_or_str(M.subst(trim(v))); goto continue end
            if #stack > 0 then k = table.concat(stack, ":") .. ":" .. k end
            local ok, e = pcall(M.keyword, k, v, { loading = true })
            if not ok then err(path .. ":" .. n .. ": " .. tostring(e)) end
        end
        ::continue::
    end
    f:close()
end

-- ── IPC ──────────────────────────────────────────────────────────────────────
_G.mgosD = function(line) return M.dispatch(line) end
_G.mgosK = function(k, v) M.keyword(k, v) end

return M
