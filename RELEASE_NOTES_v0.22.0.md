# hDeskbar v0.22.0

## New
- **Expanding tabs** option in the preferences. On (the default), a vertical bar shows the windows of a program in the box under its
  tab. Off, the tabs stay compact: a program with several windows has a chevron that opens its window list as a menu (as in a
  horizontal bar), and a program with one window needs a click on the tab only.

## Since v0.21.5
Nothing else: v0.21.5 is the release with everything the Deskbar has, see `RELEASE_NOTES_v0.21.5.md`.

## Install
Build and install it for the whole system on Arch / CachyOS, from the `linux` folder:

    cd linux && makepkg -si

On KDE Plasma log out and in again after the first install: the launcher it installs is what lets KWin give the bar its window list.
