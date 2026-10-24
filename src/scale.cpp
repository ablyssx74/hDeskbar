/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "scale.h"

#include <cmath>
#include <climits>
#include <cstring>
#include <map>

#include "fractional-scale-v1-client-protocol.h"
#include "loop.h"
#include "util.h"
#include "viewporter-client-protocol.h"

namespace {

wp_viewporter* gViewporter = nullptr;
wp_fractional_scale_manager_v1* gManager = nullptr;
int gOutputScale = 1;
double gFractional = 0;			// 0 until the compositor has said
void (*gCallback)() = nullptr;

struct Tracked {
	wp_viewport* viewport = nullptr;
	wp_fractional_scale_v1* fractional = nullptr;
};
std::map<wl_surface*, Tracked> gSurfaces;

void Preferred(void*, wp_fractional_scale_v1*, uint32_t scale)
{
	const double value = scale / 120.0;
	if (std::abs(value - gFractional) < 0.001)
		return;
	gFractional = value;
	DebugLog("fractional scale %.3f\n", value);
	if (gCallback != nullptr)
		RunLater([]() { gCallback(); });
}

const wp_fractional_scale_v1_listener kListener = { Preferred };

bool Fractional()
{
	return gManager != nullptr && gViewporter != nullptr;
}

}	// namespace

bool ScaleBindGlobal(wl_registry* registry, uint32_t name, const char* interface)
{
	if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		gViewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
		return true;
	}
	if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		gManager = static_cast<wp_fractional_scale_manager_v1*>(
			wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
		return true;
	}
	return false;
}

void ScaleSetOutput(int scale)
{
	if (scale < 1)
		scale = 1;
	if (scale == gOutputScale)
		return;
	gOutputScale = scale;
	if (!Fractional() && gCallback != nullptr)
		RunLater([]() { gCallback(); });
}

double RenderScale()
{
	if (!Fractional())
		return gOutputScale;
	return gFractional > 0 ? gFractional : gOutputScale;
}

void ScaleTrackSurface(wl_surface* surface)
{
	if (!Fractional() || gSurfaces.count(surface))
		return;
	Tracked tracked;
	tracked.viewport = wp_viewporter_get_viewport(gViewporter, surface);
	tracked.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(gManager, surface);
	wp_fractional_scale_v1_add_listener(tracked.fractional, &kListener, nullptr);
	gSurfaces[surface] = tracked;
}

void ScaleForgetSurface(wl_surface* surface)
{
	auto found = gSurfaces.find(surface);
	if (found == gSurfaces.end())
		return;
	wp_fractional_scale_v1_destroy(found->second.fractional);
	wp_viewport_destroy(found->second.viewport);
	gSurfaces.erase(found);
}

void AttachBuffer(wl_surface* surface, wl_buffer* buffer, int width, int height)
{
	auto found = gSurfaces.find(surface);
	if (found != gSurfaces.end())
		wp_viewport_set_destination(found->second.viewport, width, height);
	else
		wl_surface_set_buffer_scale(surface, std::max(1, static_cast<int>(std::lround(RenderScale()))));
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, INT_MAX, INT_MAX);
}

void ScaleOnChanged(void (*callback)())
{
	gCallback = callback;
}
