# hDeskbar

The SnakeTracker Deskbar for Linux (Wayland). A bar that looks and works like the Haiku SnakeTracker Deskbar: the lavender
panel, the pill shaped program tabs with the dark boxes of their windows, the clock, a tray, a CPU graph in the place of
ProcessController, and a menu. It is drawn with cairo on a layer-shell surface, so its corners are really round.

It is a sibling of [hDesktop](https://github.com/ablyssx74/hDesktop) (the dock) and uses the same kind of backend: the
compositor's window management protocols, StatusNotifierItem for the tray, PulseAudio for the volume.

Status: **just started** (v0.8.0). It draws the bar in any of the six places, with the header (the SnakeTracker icon on a raised
block) and its menu, the CPU graph, the clock, the volume slider, the system tray, the program tabs and a settings window.

## Places
- Vertical bars at the top left, top right, bottom left and bottom right of the screen.
- A horizontal bar along the top, or along the bottom.

A corner of the bar that is a corner of the screen stays square; every other corner is round, and the bar has the Deskbar's
one pixel outline all round.

    hdeskbar [--placement top-left|top-right|bottom-left|bottom-right|top|bottom] [--no-cpu] [--no-tray] [--no-volume] [--no-tabs] [--debug]

The flags are for one run; the settings window keeps its choices in `~/.config/hdeskbar/hdeskbar.conf`.

**Settings.** The menu of the header has Settings... : the program tabs, the system tray, the CPU graph and the volume slider can each
be shown or left out, the bar can be put in any of the six places, and the SnakeTracker color can be chosen from eight (the panel
is a light tint of it). A choice takes effect at once, even the place: the bar moves.

**The menu.** A click on the header opens it: About, Applications (by category: Sound & Video, Development, Games, Graphics, Internet,
Office, Science, Settings, System, Accessories, Other), Power (lock, log out, restart, shut down: the first two through `loginctl`, the
others through `systemctl`) and Quit. It opens downward from a bar at the top of the screen and upward from one at the bottom; a
long list scrolls with the wheel; a press outside closes it. Each menu is an `xdg_popup`, so the compositor puts it where it fits.

**Program tabs.** One tab for each program that has windows, in the order they first appeared, with the program's icon and name.
In a vertical bar the windows of a program are listed in a dark box under its tab (the chevron closes and opens the box); in a
horizontal bar the tabs are side by side. Clicking a tab brings the program's last window to the front (a lone window that is
already in front is minimized); clicking a window in the box brings that window up. The shapes are straight where the bar is at
the edge of the screen, and round at the other end, mirrored for a bar on the left.

The window list comes from `wlr-foreign-toplevel-management` (Hyprland, Sway, niri, labwc, Wayfire...) or, on KDE, from
`org_kde_plasma_window_management`, which KWin gives only to a program installed with a `.desktop` file that asks for it:
on KDE run `sudo make install` first. (`data/hdeskbar.desktop.in` is that file.)

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
4. **Program tabs** (done) from the compositor's window list, with icons; click to focus.
5. **The expanded view** (done) of the vertical bars: the dark box with each program's windows under its tab, in the shapes of
   SnakeTracker 1.0.51 (straight at the screen's edge, round at the other end).
6. **The horizontal bars** (the tabs side by side are done; the list of a program's windows under its tab is not).
7. **The menu** (done in a first form): the header opens a menu with the applications by category, power actions and quit.
   Still to come: the snake trail joining the submenus, the tray's own (dbusmenu) menus, the CPU graph's menu, the window list of a
   program in a horizontal bar, and tooltips.
8. **Settings** (done), and a PKGBUILD.

The Haiku Deskbar's four small corner layouts are not planned.

## Build
Needs `wayland`, `cairo`, `pango`, `librsvg`, `gdk-pixbuf2` and `glib2` and `libpulse` (Arch / CachyOS package names).

    make
    ./hdeskbar --placement top-right

Needs a compositor with `wlr-layer-shell` (KWin, Hyprland, Sway, niri, labwc...).

## License
MIT, see `LICENSE`.
