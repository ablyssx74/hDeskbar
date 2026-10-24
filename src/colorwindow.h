/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The color picker, as hDesktop's: a square for the saturation and brightness, a strip for the hue, the colors it has to offer, and
 * Default, Cancel and Apply. The color changes where it is used while it is being chosen.
 */
#ifndef HDESKBAR_COLORWINDOW_H
#define HDESKBAR_COLORWINDOW_H

#include <wayland-client.h>

#include <cstdint>
#include <functional>
#include <string>

#include "style.h"

// onChange gets the color as it is chosen (and the old one back on Cancel); onDone(true) says it was applied.
void OpenColorWindow(const std::string& title, const Color& current, const Color& defaultColor,
	std::function<void(const Color&)> onChange, std::function<void(bool)> onDone);

bool ColorWindowOpen();
bool ColorWindowOwnsSurface(wl_surface* surface);
void ColorWindowPointerEnter(double x, double y);
void ColorWindowPointerLeave();
void ColorWindowPointerMotion(double x, double y);
void ColorWindowPointerButton(uint32_t button, uint32_t state);

#endif
