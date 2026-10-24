/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_SHM_H
#define HDESKBAR_SHM_H

#include <cairo/cairo.h>
#include <wayland-client.h>

#include <functional>

// A buffer of width x height pixels, drawn by draw() with cairo (on a transparent background). The buffer is destroyed when the
// compositor has done with it.
wl_buffer* NewShmBuffer(int width, int height, const std::function<void(cairo_t*)>& draw);

#endif
