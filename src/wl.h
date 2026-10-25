/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * What the bar and its popups share of the Wayland connection.
 */
#ifndef HDESKBAR_WL_H
#define HDESKBAR_WL_H

#include <wayland-client.h>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

struct WlGlobals {
	wl_display* display = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	wl_seat* seat = nullptr;
	xdg_wm_base* wmBase = nullptr;
	zxdg_decoration_manager_v1* decorationManager = nullptr;		// lets a window draw its own frame
	wl_surface* barSurface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	uint32_t serial = 0;				// of the last button press or release: a popup grabs with it
	wl_surface* pointerSurface = nullptr;		// the surface the pointer is over
	int screenHeight = 0;
};

extern WlGlobals gWl;

#endif
