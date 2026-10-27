# hDeskbar v0.25.0

## New
- **Browse** is a new item of the menu, right above Recent documents. It has two folders, **/** and **Home**, that open as menus of their contents like the Tracker menu of hDesktop: the folders first, then the files, hidden ones left out; hover a folder to open it as a submenu, click a folder to open it in your file manager, click a file to open it in its program. Folders have the icons of your theme (home, Documents, Downloads and so on), files the icon of their type. A folder with more than 1000 entries ends with a row that opens the folder itself.
- **The Mount menu shows the volumes that are not mounted,** dimmed, below the mounted ones (which look as before). Choosing one mounts it with udisks and opens it. They used to be missing unless gvfs' udisks monitor was running; they are now listed with `lsblk` (swap, LVM, encrypted, RAID and loop devices are left out). An internal disk may ask for your password the first time, as the file manager does.
- **No light line along the top of the header,** so the snake trail of the menu joins the header better.

## Install
From the `linux` folder: `makepkg -si`
