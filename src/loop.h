/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The bar runs on one GLib main loop: D-Bus for the tray, timers, and the Wayland socket as a source of its own.
 */
#ifndef HDESKBAR_LOOP_H
#define HDESKBAR_LOOP_H

#include <glib.h>

#include <functional>

struct wl_display;

// Plugs the Wayland connection into the main loop (the standard prepare_read / read_events handshake); onLost is called when
// the connection goes away or fails.
void AttachWaylandSource(wl_display* display, std::function<void()> onLost);

// Calls fn on the next pass of the main loop (a click handler that wants to change the thing that delivered the click).
void RunLater(std::function<void()> fn);

// Calls fn once, after ms milliseconds.
guint RunAfter(unsigned ms, std::function<void()> fn);

#endif
