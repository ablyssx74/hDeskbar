# hDeskbar

The SnakeTracker Deskbar for Linux (Wayland). A bar that looks and works like the Haiku SnakeTracker Deskbar: the lavender
panel, the pill shaped program tabs with the dark boxes of their windows, the clock, a tray, a CPU graph in the place of
ProcessController, and a menu. It is drawn with cairo on a layer-shell surface, so its corners are really round.

It is a sibling of [hDesktop](https://github.com/ablyssx74/hDesktop) (the dock) and uses the same kind of backend: the
compositor's window management protocols, StatusNotifierItem for the tray, PulseAudio for the volume.

Status: **just started** (v0.21.5). It draws the bar in any of the six places, with the header (the SnakeTracker icon on a raised
block) and its menu, the CPU graph, the clock, the volume icon, the system tray (with the programs' own menus), the program tabs and a settings window.

## Places
- Vertical bars at the top left, top right, bottom left and bottom right of the screen.
- A horizontal bar along the top, or along the bottom.

A corner of the bar that is a corner of the screen stays square; every other corner is round, and the bar has the Deskbar's
one pixel outline all round.

    hdeskbar [--placement top-left|top-right|bottom-left|bottom-right|top|bottom] [--no-cpu] [--no-tray] [--no-volume] [--no-tabs] [--debug]

The flags are for one run; the settings window keeps its choices in `~/.config/hdeskbar/hdeskbar.conf`.

**Settings.** The menu of the header has Settings... : the program tabs, the system tray, the CPU graph and the volume icon can each
be shown or left out ("Volume icon" among them), it checks GitHub for a newer version (a notification), the clock can be 24-hour (13:49) and show seconds, the bar can be put in any of the six places, kept always on top or behind the windows (with auto-raise: it comes over them when the pointer is at its edge), or auto-hidden (an invisible strip at the edge of the screen, until the pointer is there), and the SnakeTracker color is chosen in a picker (a square, a hue strip, twenty presets; the panel
is a light tint of it). A choice takes effect at once, even the place: the bar moves.

**The menu.** A click on the header opens the menu, laid out like the Haiku Deskbar's leaf menu, and beside a bar at the side of the
screen (to the left of one on the right, to the right of one on the left; below or above a bar along an edge): About, Find (the
launcher of your desktop: KRunner, rofi, wofi, fuzzel...), Show tray icons (a check mark), Mount (the volumes: choosing one opens it, and
mounts it first when it is not mounted), Deskbar preferences, Quit; Shutdown (Lock Screen, Log Out, Restart, Shut Down: the first two
through `loginctl`, the others through `systemctl`); Recent documents and Recent folders (the GTK list and KDE's), Recent
applications (the ones this menu started); and Applications (by category) and Preferences (the settings programs). The row of
a menu that has a submenu open stays lit and runs on to the submenu, and a strip along the submenu's edge runs from its top to
that row: the snake trail. A long list scrolls with the wheel; a press outside closes the menus. Each menu is an `xdg_popup`.

**The CPU graph's menu** (a right click on the graph), as ProcessController's and as hDesktop's: **Quit an application** (the programs
you are running, without the ones the session needs; it asks first, and sends them SIGTERM), **Memory usage** (the system's memory,
and every process with what it has written to and all the RAM it has, in a bar), **Processes and CPU usage** (every process, the
busiest first, with a bar of what it takes; each one opens its threads, with their own bars and nice values to choose, and
**Force Kill**, which asks first) and **Power saving** (when `power-profiles-daemon` is there). The lists follow the system while
they are open.

**Tooltips.** Rest the pointer on something for half a second and a dark capsule says what it is: the CPU load on the graph, the date
on the clock, the level on the volume icon, the name of a tray icon, a program's name (and how many windows it has, or the title of its
only one) on its tab, the whole title of a window in the box. They are popups that take no input, so a click goes through to the
bar, and they go away when the pointer moves off or a button is pressed.

**The window list.** A program with several windows has a small chevron on its tab in a horizontal bar, and a click on the tab opens
the list of its windows: the one in front has a dot, a hidden one is in parentheses, and Hide all, Show all and Close all are under
them. A right click on any tab, in any bar, opens the same list. (In a vertical bar a left click brings the program's last window up,
as the windows are in the box under the tab.)

**Program tabs.** One tab for each program that has windows, in the order they first appeared, with the program's icon and name.
In a vertical bar the windows of a program are listed in a dark box under its tab (the chevron closes and opens the box); in a
horizontal bar the tabs are side by side. Clicking a tab brings the program's last window to the front (a lone window that is
already in front is minimized); clicking a window in the box brings that window up. The shapes are straight where the bar is at
the edge of the screen, and round at the other end, mirrored for a bar on the left.

The window list comes from `wlr-foreign-toplevel-management` (Hyprland, Sway, niri, labwc, Wayfire...) or, on KDE, from
`org_kde_plasma_window_management`, which KWin gives only to a program installed with a `.desktop` file that asks for it:
on KDE run `sudo make install` first. (`data/hdeskbar.desktop.in` is that file.)

The volume is an icon in the tray's row: a speaker of the icon theme, with more or fewer waves for how loud it is and a cross when it
is muted. A click on it mutes (and brings the sound back), the wheel over it changes the volume two percent a notch, and a right click
has Mute and the mixer (`pavucontrol`, or the KDE sound settings). It follows the volume of the default output through PulseAudio,
whoever changes it.

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

The volume is an icon in the tray's row: a speaker of the icon theme, with more or fewer waves for how loud it is and a cross when it
is muted. A click on it mutes (and brings the sound back), the wheel over it changes the volume two percent a notch, and a right click
has Mute and the mixer (`pavucontrol`, or the KDE sound settings). It follows the volume of the default output through PulseAudio,
whoever changes it.

The tray is a StatusNotifierHost (and the watcher too when nobody else is): a left click activates an icon, a middle click is its
secondary action, and a right click opens its menu. The menu a program describes over D-Bus (dbusmenu) is drawn by hDeskbar in the
look of its own menus, with the icons, check marks, radio dots, separators and submenus; a program that has none is asked for its
own (ContextMenu).

## Plan
1. **Skeleton** (done): the layer-shell surface in the six places, the outline and the round corners.
2. **Header, clock, CPU graph, tray** (done). A centered SnakeTracker icon as the header (it opens the menu), the clock, the tray
   (StatusNotifierItem), and hDesktop's CPU graph in the place of ProcessController. The tray, the CPU graph and the volume
   control can each be turned off.
3. **Volume** (done): hDesktop's PulseAudio code, as an icon (KDE shows its own overlay when the volume changes, so there is no slider).
4. **Program tabs** (done) from the compositor's window list, with icons; click to focus.
5. **The expanded view** (done) of the vertical bars: the dark box with each program's windows under its tab, in the shapes of
   SnakeTracker 1.0.51 (straight at the screen's edge, round at the other end).
6. **The horizontal bars** (done): the tabs side by side, and the list of a program's windows as a menu under its tab.
7. **The menu** (done in a first form): the header opens a menu with the applications by category, power actions and quit.

8. **Settings** (done), and a PKGBUILD (done).

The Haiku Deskbar's four small corner layouts are not planned.

## Build
Needs `wayland`, `cairo`, `pango`, `librsvg`, `gdk-pixbuf2` and `glib2` `libpulse` and `curl` (Arch / CachyOS package names).

    make
    ./hdeskbar --placement top-right

## Install (Arch / CachyOS)
    git clone https://github.com/ablyssx74/hDeskbar.git
    cd hDeskbar/linux
    makepkg -si

This installs `hdeskbar` in `/usr/bin`, with its launcher and icon (on KDE Plasma the launcher is what lets KWin give it the window list).

Needs a compositor with `wlr-layer-shell` (KWin, Hyprland, Sway, niri, labwc...).

## License
MIT, see `LICENSE`.
