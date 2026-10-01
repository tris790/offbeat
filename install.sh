#!/usr/bin/env bash
#
# Install Offbeat for the current user (no root):
#   ~/.local/bin/offbeat -> <repo>/build/offbeat   launcher symlink
#   ~/.local/share/applications/offbeat.desktop
#   ~/.local/share/icons/hicolor/*/apps/offbeat.png
#
# The launcher symlinks the build output (assets are found via ../assets), so
# the taskbar always runs your latest build -- including debug ./build.sh runs.
#
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
./build.sh debug

PREFIX="${PREFIX:-$HOME/.local}"
mkdir -p "$PREFIX/bin" "$PREFIX/share/applications" "$PREFIX/share/icons"
ln -sf "$ROOT/build/offbeat" "$PREFIX/bin/offbeat"
# absolute Exec: launchers often lack ~/.local/bin in PATH
sed "s|^Exec=.*|Exec=$PREFIX/bin/offbeat|" assets/offbeat.desktop > "$PREFIX/share/applications/offbeat.desktop"
cp -r assets/icons/hicolor "$PREFIX/share/icons/"
gtk-update-icon-cache -q "$PREFIX/share/icons/hicolor" 2>/dev/null || true
update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true
echo "launcher -> $ROOT/build/offbeat (run: offbeat)"
