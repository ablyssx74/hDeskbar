# hDeskbar

The SnakeTracker Deskbar for Linux (Wayland). A bar that looks and works like the Haiku SnakeTracker Deskbar: the lavender
panel, the pill shaped program tabs with the dark boxes of their windows, the clock, a tray, a CPU graph in the place of
ProcessController, and a menu. It is drawn with cairo on a layer-shell surface, so its corners are really round.

It is a sibling of [hDesktop](https://github.com/ablyssx74/hDesktop) (the dock) and uses the same kind of backend: the
compositor's window management protocols, StatusNotifierItem for the tray, PulseAudio for the volume.

Status: **just started** (v0.5.0). It draws the bar in any of the six places, with the header (the SnakeTracker icon on a raised
block), the CPU graph, the clock, the volume slider and the system tray; nothing else is there yet.

## Places
- Vertical bars at the top left, top right, bottom left and bottom right of the screen.
- A horizontal bar along the top, or along the bottom.

A corner of the bar that is a corner of the screen stays square; every other corner is round, and the bar has the Deskbar's
one pixel outline all round.

    hdeskbar --placement top-left|top-right|bottom-left|bottom-right|top|bottom [--no-cpu] [--no-tray] [--no-volume] [--debug]

`--no-cpu` leaves the CPU graph out, `--no-tray` the tray and `--no-volume` the volume slider (settings will do that later).

The volume slider has a speaker at its left (click it to mute) and a groove with a knob: press and drag, or turn the wheel over
it (two percent a notch). It follows the volume of the default output through PulseAudio, whoever changes it.

The tray is a StatusNotifierHost (and the watcher too when nobody else is): left click activates an icon, right click opens its
menu through the program's own ContextMenu call, middle click is the secondary action. The menus the programs describe over
D-Bus (dbusmenu) will be drawn by hDeskbar itself when the menu code is in.

## Plan
1. **Skeleton** (done): the layer-shell surface in the six places, the outline and the round corners.
2. **Header, clock, CPU graph, tray** (done). A centered SnakeTracker icon as the header (it opens the menu), the clock, the tray
   (StatusNotifierItem), and hDesktop's CPU graph in the place of ProcessController. The tray, the CPU graph and the volume
   control can each be turned off.
3. **Volume control** (done): hDesktop's PulseAudio code, with a slider like the Haiku Deskbar's.
4. **Program tabs** from the compositor's window list, with icons; click to focus.
5. **The expanded view** of the vertical bars: the dark box with each program's windows under its tab, in the shapes of
   SnakeTracker 1.0.51 (straight at the screen's edge, round at the other end).
6. **The horizontal bars**, with the tabs side by side.
7. **The menu** (an application menu built from `.desktop` files, in the snake trail look), opened from the header.
8. **Settings**, and a PKGBUILD.

The Haiku Deskbar's four small corner layouts are not planned.

## Build
Needs `wayland`, `cairo`, `pango`, `librsvg`, `gdk-pixbuf2` and `glib2` and `libpulse` (Arch / CachyOS package names).

    make
    ./hdeskbar --placement top-right

Needs a compositor with `wlr-layer-shell` (KWin, Hyprland, Sway, niri, labwc...).

## License
MIT, see `LICENSE`.
