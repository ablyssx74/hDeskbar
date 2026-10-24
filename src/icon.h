/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_ICON_H
#define HDESKBAR_ICON_H

#include <cairo/cairo.h>

#include "icons.h"

// The SnakeTracker icon (the server's), drawn size x size with its top left at x, y.
void DrawSnakeTrackerIcon(cairo_t* cr, double x, double y, int size);

// The same snake as a picture for a menu item (size logical pixels square).
IconRef AboutMenuIcon(int size);

#endif
