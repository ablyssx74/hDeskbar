/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * A window with a question and some buttons (are you sure you want to kill it?), in the look of the bar.
 */
#ifndef HDESKBAR_ALERT_H
#define HDESKBAR_ALERT_H

#include <wayland-client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// onChoice is told the number of the button pressed (0 for the first); closing the window with its own button counts as the first.
void ShowAlert(const std::string& title, const std::string& text, std::vector<std::string> buttons,
	std::function<void(int)> onChoice, int defaultButton = -1);

bool AlertOwnsSurface(wl_surface* surface);
void AlertPointerEnter(wl_surface* surface, double x, double y);
void AlertPointerLeave();
void AlertPointerMotion(double x, double y);
void AlertPointerButton(uint32_t button, uint32_t state);

#endif
