/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The scale of the screen (125%, 200%, ...): the surfaces keep their size in logical pixels, and the buffers have more pixels.
 * A compositor with the fractional scale protocol tells the scale to the 1/120th; without it the output's whole number is used.
 */
#ifndef HDESKBAR_SCALE_H
#define HDESKBAR_SCALE_H

#include <wayland-client.h>

#include <cstdint>

// Called from the registry: takes the globals that have to do with the scale. Returns whether it was one.
bool ScaleBindGlobal(wl_registry* registry, uint32_t name, const char* interface);

// The whole number of the output (used when there is no fractional scale).
void ScaleSetOutput(int scale);

// What the buffers are drawn at.
double RenderScale();

// A surface that gets a buffer made at RenderScale() is put on the list, before it has its first buffer, and taken off before it is destroyed.
void ScaleTrackSurface(wl_surface* surface);
void ScaleForgetSurface(wl_surface* surface);

// attach + damage, for a buffer made with NewShmBuffer for a surface of width x height logical pixels. The caller commits.
void AttachBuffer(wl_surface* surface, wl_buffer* buffer, int width, int height);

// Said when the scale has changed (the bar draws again).
void ScaleOnChanged(void (*callback)());

#endif
