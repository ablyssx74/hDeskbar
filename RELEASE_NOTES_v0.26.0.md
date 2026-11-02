# hDeskbar v0.26.0

## New
- **The preferences window has tabs** (Show, Behavior, Look and Header), so it is smaller and fits small screens.
- **A background color**, next to the highlight color (the SnakeTracker color). "Match the highlight" keeps the old look, where the background is a light tint of the highlight.
- **Transparency for both colors**: an opacity slider for each, from 20% to 100%. It applies to the bar and its menus.
- **A different icon for the header.** Choose a picture (PNG, SVG, JPEG or XPM) with the file chooser of the desktop (kdialog or zenity), or go back to the SnakeTracker snake with Default.
- **The size of the header and its icon**: 16, 24, 32, 48 or 64, as in the Haiku Deskbar. The bar grows with it.
- **Mount shows the names of the volumes**, not their serial numbers: the label of the volume, else the name of its partition, else the make of the disk and its size ("System" for the root). Volumes that are not mounted show their size.

## Install
From the `linux` folder: `makepkg -si`
