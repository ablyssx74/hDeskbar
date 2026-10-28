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
// (widest: the longest text it will have, when it changes while it is shown: the capsule is as wide as that, so it does not
// change its size as the numbers change)
void ShowTooltip(const std::string& text, int x, int y, int w, int h, bool above, const std::string& widest = std::string());
// A shown tooltip says something else now (the CPU load, the volume): it is drawn again in place.
void UpdateTooltipText(const std::string& text);
void HideTooltip();
bool TooltipShown();

#endif
