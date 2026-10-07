#!/bin/sh
# Installs a launcher + icons for whyhot-desktop into ~/.local/share so it
# shows up in the app grid and can be pinned to the dock.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/build/whyhot-desktop"
[ -x "$bin" ] || { echo "build first: cmake --build build" >&2; exit 1; }

share="${XDG_DATA_HOME:-$HOME/.local/share}"
for size in 48 64 128 256 512; do
  dir="$share/icons/hicolor/${size}x${size}/apps"
  mkdir -p "$dir"
  QT_QPA_PLATFORM=offscreen "$bin" --icon "$dir/whyhot.png" "$size"
done

mkdir -p "$share/applications"
cat > "$share/applications/whyhot.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=whyhot
GenericName=Fan & heat companion
Comment=Why is my fan running?
Exec=$bin
Icon=whyhot
Terminal=false
Categories=System;Monitor;
StartupWMClass=whyhot
DESKTOP

command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$share/icons/hicolor" || true
command -v update-desktop-database >/dev/null && update-desktop-database -q "$share/applications" || true
echo "Installed. Search for 'whyhot' in Activities, launch it, then right-click its dock icon > Add to Favorites."
