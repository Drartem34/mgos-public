#!/usr/bin/env bash
# kioskOS — Nautilus Cache & Memory Trimmer
# Limits thumbnail cache to avoid SSD bloat

THUMB_DIR="$HOME/.cache/thumbnails"
if [ -d "$THUMB_DIR" ]; then
    # Remove failed thumbnails
    rm -rf "$THUMB_DIR/fail"/* 2>/dev/null || true
    # Remove thumbnails older than 14 days
    find "$THUMB_DIR" -type f -atime +14 -delete 2>/dev/null || true
fi

# Clean rclone stale locks if any
rm -f "$HOME/.cache/rclone/vfsMeta"/*.lock 2>/dev/null || true
