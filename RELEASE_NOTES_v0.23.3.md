# hDeskbar v0.23.3

## New
- **A vertical bar runs flush into the side of the screen it hangs on.** The header and the program tabs reach the edge, there is no outline along the screen's edges, and both corners on that side are square. The outline stays on the sides that face into the screen.
- **At the bottom of the screen the header comes last.** The bottom-left and bottom-right bars start at the edge they hang on: the header at the bottom, then the clock and graph, the tray, and the program tabs above them. A tab's windows stay under its tab, and the newest tab is the one next to the tray. The leaf menu opens upward from the header, with its snake trail starting at the bottom edge of the bar. A bar too tall for the screen is cut off at its far end, so the header stays.
- **A larger icon in the header.** 24 pixels in a vertical bar (22 in a horizontal one).
- **The About window** has round corners (it draws its own frame, and is moved by dragging it anywhere that is not the Close button), is 20% wider, and has a larger terminal box and text. It has a gold title with a shadow, a shadow behind the logo, the subtitle in the middle, and new wording.

Horizontal bars are as they were.

## Install
Build and install it for the whole system on Arch / CachyOS, from the `linux` folder:

    cd linux && makepkg -si

On KDE Plasma log out and in again after the first install: the launcher it installs is what lets KWin give the bar its window list.

## Notes
- Moving the About window by dragging, the tooltips and the window lists of a bar at the bottom of the screen, and the new layout on a real desktop, have not been looked at; they were checked in a nested KWin only.
