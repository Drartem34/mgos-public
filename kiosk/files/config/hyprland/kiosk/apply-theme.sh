#!/usr/bin/env bash
set -e

THEME_DIR="$HOME/.config/hyprland/kiosk"
GTK4_DIR="$HOME/.config/gtk-4.0"
GTK3_DIR="$HOME/.config/gtk-3.0"

echo "==> Deploying Graphite Theme to GTK 4.0 and GTK 3.0..."
mkdir -p "$GTK4_DIR" "$GTK3_DIR"

# Clean up broken Materia links and old backups
rm -f "$GTK4_DIR/assets" "$GTK4_DIR/gtk.css" "$GTK4_DIR/gtk-dark.css" "$GTK4_DIR/gtk.css.save"*
rm -f "$GTK3_DIR/gtk.css"

# Link Graphite theme
ln -sf "$THEME_DIR/graphite.css" "$GTK4_DIR/gtk.css"
ln -sf "$THEME_DIR/graphite.css" "$GTK4_DIR/gtk-dark.css"
ln -sf "$THEME_DIR/gtk-3.0.css" "$GTK3_DIR/gtk.css"

# Copy settings
cp "$THEME_DIR/settings.ini" "$GTK4_DIR/settings.ini"
cp "$THEME_DIR/settings.ini" "$GTK3_DIR/settings.ini"

# Add Google Drive to bookmarks if not present
BOOKMARKS_FILE="$GTK3_DIR/bookmarks"
if ! grep -q "GoogleDrive" "$BOOKMARKS_FILE" 2>/dev/null; then
    echo "file://$HOME/GoogleDrive Google Drive" >> "$BOOKMARKS_FILE"
    echo "==> Added Google Drive to sidebar bookmarks"
fi

echo "==> Configuring GNOME desktop & Nautilus performance settings..."
gsettings set org.gnome.desktop.interface color-scheme 'prefer-dark'
gsettings set org.gnome.desktop.interface accent-color 'slate'
gsettings set org.gnome.desktop.interface gtk-theme 'Adwaita-dark'

# Nautilus instant speed & no freeze optimizations
gsettings set org.gnome.nautilus.preferences fts-enabled false
gsettings set org.gnome.nautilus.preferences show-directory-item-counts 'never'
gsettings set org.gnome.nautilus.preferences recursive-search 'never'
gsettings set org.gnome.nautilus.preferences show-image-thumbnails 'local-only'
gsettings set org.gnome.nautilus.preferences thumbnail-limit 10

# Clear broken thumbnail cache
rm -rf "$HOME/.cache/thumbnails/fail"/* 2>/dev/null || true

# Mask broken localsearch systemd units (prevents DBus activation hang)
systemctl --user mask localsearch-3.service localsearch-control-3.service localsearch-writeback-3.service 2>/dev/null || true

echo "==> Restarting Nautilus..."
nautilus -q 2>/dev/null || true

echo "==> Graphite theme deployed successfully!"
