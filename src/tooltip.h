/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Tooltips: a dark capsule with a line of text, a popup of its own that takes no input (the pointer goes through it to the bar).
 */
#ifndef HDESKBAR_TOOLTIP_H
#define HDESKBAR_TOOLTIP_H

#include <string>

// Shows text beside the rectangle (x, y, w, h) of the bar, below it or above it, centered on it.
void ShowTooltip(const std::string& text, int x, int y, int w, int h, bool above);
void HideTooltip();
bool TooltipShown();

#endif
