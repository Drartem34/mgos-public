---
title: Hyprland 0.57 runs mgOS on a Lua config plus a translator for the old config files and old hyprctl commands
author: Doki
date: 2026-11-21
branch: main
id: l-91a1fd1e
scope: repo
status: active
src: agent
touches:
  - configs/hypr/hyprland.lua
  - configs/hypr/mgos/legacy.lua
  - configs/SYSui/sysui_common.h
  - gui/steps.sh
---

## Decided
- Hyprland 0.57 removed the old config language: hyprland.conf is ignored and old-style "hyprctl dispatch X" / "keyword K V" fail. The main config was rewritten in Lua, but the small files that Settings, the installer, mgos-bakeblur and pywal generate stay in the old format and are read through a translator module. That way none of those generators had to change.
- Old IPC commands are translated in one place for scripts (a hyprctl wrapper in ~/.local/bin, which comes first in PATH) and one place for C++ (sysui::hypr_request). Both translate only when Hyprland ≥ 0.57, so machines still on 0.56 keep working.
- The Lua config adapts to the hardware itself (NVIDIA env only if /proc/driver/nvidia exists, autostart skips missing programs and paths) instead of the installer patching it with sed.
- GTK ≥ 4.24 turns off compositor blur for every window through ext-background-effect-v1. GDK_WAYLAND_DISABLE=ext_background_effect_manager_v1 gives blur control back to Hyprland's layer and window rules.

## Tried and ruled out
- Downgrading Hyprland to 0.56: the old packages were not in the pacman cache, and the user chose Lua.
- Widening the dock font for the RAM icon: not the cause. The icon fell back to a font that has a different glyph at U+EFC5 because "Symbols Nerd Font" is not installed.

## Watch out
- Building a dispatcher is safe; hl.dispatch / hyprctl dispatch runs it on the active window. Never probe the API by dispatching.
- hl.gesture "unset" on a gesture that doesn't exist is reported as a config error, not thrown, so pcall doesn't catch it. Track which gestures were set.
- Some options changed type: drag_lock is now numeric while Settings writes true/false; render_power above 4 is now an error instead of being clamped.
- No workspaceopt (allfloat) in 0.57.
- kioskOS (kiosk.conf, kiosk-dock, runner) is still old-format and won't start on 0.57.

In the code:
- configs/hypr/hyprland.lua, configs/hypr/mgos/legacy.lua (globals mgosD, mgosK, mgos_autostart)
- home/.local/bin/hyprctl, home/.local/bin/mgos-bakeblur
- configs/SYSui/sysui_common.h: hypr_is_lua, hypr_legacy_translate
- gui/steps.sh: _hypr_lua, Lua checks in step_verify
- Validate: Hyprland --verify-config -c ~/.config/hypr/hyprland.lua
