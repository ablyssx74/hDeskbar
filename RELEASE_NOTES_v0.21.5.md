# hDeskbar v0.21.5

The SnakeTracker Deskbar for Wayland: a bar in the look of the Haiku Deskbar for KDE Plasma, Hyprland, Sway, niri, labwc and any
compositor with `wlr-layer-shell`.

## What it has
- **Six places**: top-left, top-right, bottom-left and bottom-right as a vertical bar, or along the top or the bottom of the screen.
- **The header**: the SnakeTracker icon, the clock (12 or 24 hour, with or without seconds), the CPU graph and the volume icon.
- **The leaf menu**: About, Find, tray icons, Mount, Deskbar preferences, Shutdown, Recent documents, folders and applications, and
  Applications. It opens beside a vertical bar, or below or above a horizontal one, and its snake trail starts at the bar.
- **Program tabs** with a window list for each program (Hide all, Show all, Close all).
- **System tray** with the programs' own menus.
- **CPU graph menu** (right click): quit an application, memory usage, processes and threads, and Force Kill.
- **Menus** as pills with the snake trail through every level, as in hDesktop's Tracker. With the bar at the right they open to
  the left, and their chevrons point left.
- **Preferences**: what to show, the place, the SnakeTracker color (a picker, as hDesktop's), Always on top, Auto-raise and
  Auto-hide, and a check for updates.
- **HiDPI**: fractional scales (125%, 150%...) with sharp text, icons and menus.
- **About window** with the snake, and a little shell with the thanks.

## Install
Build and install it for the whole system on Arch / CachyOS, from the `linux` folder:

    cd linux && makepkg -si

On KDE Plasma log out and in again after the first install: the launcher it installs is what lets KWin give the bar its window list.

## Notes
- A vertical bar does not yet keep windows clear of itself (a horizontal one does, when it is always on top and not auto-hidden).
- The update check reads the `VERSION` file of this repository.
