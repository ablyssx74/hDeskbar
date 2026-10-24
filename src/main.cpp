/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * hdeskbar: the SnakeTracker Deskbar for Wayland.
 *
 * This is the first step: a bar on a layer-shell surface, in any of the six places the Deskbar can sit (the four corners
 * as a vertical bar, the top and the bottom as a horizontal one), with the Deskbar's outline and its rounded corners.
 * A corner that is a corner of the screen stays square, any other is round.
 */

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"

namespace {

enum class Placement { TopLeft, TopRight, BottomLeft, BottomRight, Top, Bottom };

struct Color { double r, g, b; };

constexpr Color kPanel = {200 / 255.0, 203 / 255.0, 235 / 255.0};	// the lavender panel
constexpr Color kOutline = {20 / 255.0, 22 / 255.0, 30 / 255.0};	// the line round the bar
constexpr Color kAccent = {70 / 255.0, 110 / 255.0, 200 / 255.0};	// the SnakeTracker color
constexpr double kRadius = 7;

struct App {
	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	zwlr_layer_shell_v1* layerShell = nullptr;
	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;

	Placement placement = Placement::TopRight;
	int width = 150, height = 110;		// what the bar asks for (a horizontal bar's width is the screen's)
	bool running = true;
	bool configured = false;
};

App gApp;

bool IsVertical(Placement p)
{
	return p != Placement::Top && p != Placement::Bottom;
}

// ---- drawing ----

void RoundedRectPath(cairo_t* cr, double x, double y, double w, double h, double tl, double tr, double br, double bl)
{
	cairo_new_sub_path(cr);
	cairo_move_to(cr, x + tl, y);
	cairo_line_to(cr, x + w - tr, y);
	if (tr > 0)
		cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
	cairo_line_to(cr, x + w, y + h - br);
	if (br > 0)
		cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
	cairo_line_to(cr, x + bl, y + h);
	if (bl > 0)
		cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
	cairo_line_to(cr, x, y + tl);
	if (tl > 0)
		cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void Draw(cairo_t* cr, int width, int height)
{
	// a corner that is a corner of the screen stays square; any other is round
	double tl = kRadius, tr = kRadius, br = kRadius, bl = kRadius;
	switch (gApp.placement) {
		case Placement::TopLeft: tl = 0; break;
		case Placement::TopRight: tr = 0; break;
		case Placement::BottomLeft: bl = 0; break;
		case Placement::BottomRight: br = 0; break;
		case Placement::Top: tl = tr = 0; break;
		case Placement::Bottom: bl = br = 0; break;
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	// the panel, and the outline over its edge (half a pixel in, so the line is one pixel wide)
	RoundedRectPath(cr, 0, 0, width, height, tl, tr, br, bl);
	cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
	cairo_fill_preserve(cr);
	cairo_new_path(cr);
	RoundedRectPath(cr, 0.5, 0.5, width - 1, height - 1, std::max(0.0, tl - 0.5), std::max(0.0, tr - 0.5),
		std::max(0.0, br - 0.5), std::max(0.0, bl - 0.5));
	cairo_set_source_rgb(cr, kOutline.r, kOutline.g, kOutline.b);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	// the header, for now: a block in the SnakeTracker color with the name
	const double headerHeight = IsVertical(gApp.placement) ? 24 : height - 4;
	const double headerWidth = IsVertical(gApp.placement) ? width - 2 : 90;
	cairo_set_source_rgb(cr, kAccent.r, kAccent.g, kAccent.b);
	RoundedRectPath(cr, 1, 1, headerWidth, headerHeight, tl > 0 ? tl - 1 : 0, 0, 0, 0);
	cairo_fill(cr);

	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string("Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, "hDeskbar " APP_VERSION, -1);
	int textWidth = 0, textHeight = 0;
	pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, 1 + (headerWidth - textWidth) / 2, 1 + (headerHeight - textHeight) / 2);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(font);
	g_object_unref(layout);
}

// ---- the buffer ----

wl_buffer* MakeBuffer(int width, int height)
{
	const int stride = width * 4;
	const int size = stride * height;
	int fd = memfd_create("hdeskbar", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return nullptr;
	void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return nullptr;
	}
	cairo_surface_t* surface = cairo_image_surface_create_for_data(
		static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, width, height, stride);
	cairo_t* cr = cairo_create(surface);
	Draw(cr, width, height);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);

	wl_shm_pool* pool = wl_shm_create_pool(gApp.shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(data, size);
	close(fd);
	return buffer;
}

void Present(int width, int height)
{
	wl_buffer* buffer = MakeBuffer(width, height);
	if (buffer == nullptr)
		return;
	wl_surface_attach(gApp.surface, buffer, 0, 0);
	wl_surface_damage_buffer(gApp.surface, 0, 0, width, height);
	wl_surface_commit(gApp.surface);
}

// ---- layer surface ----

void LayerConfigure(void*, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
	if (width > 0)
		gApp.width = width;
	if (height > 0)
		gApp.height = height;
	gApp.configured = true;
	Present(gApp.width, gApp.height);
}

void LayerClosed(void*, zwlr_layer_surface_v1*)
{
	gApp.running = false;
}

const zwlr_layer_surface_v1_listener kLayerListener = { LayerConfigure, LayerClosed };

// ---- registry ----

void RegistryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		gApp.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		gApp.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
	else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
		gApp.layerShell = static_cast<zwlr_layer_shell_v1*>(
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
}

void RegistryRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = { RegistryGlobal, RegistryRemove };

bool ParsePlacement(const std::string& name, Placement* placement)
{
	static const struct { const char* name; Placement placement; } kNames[] = {
		{"top-left", Placement::TopLeft}, {"top-right", Placement::TopRight},
		{"bottom-left", Placement::BottomLeft}, {"bottom-right", Placement::BottomRight},
		{"top", Placement::Top}, {"bottom", Placement::Bottom},
	};
	for (const auto& entry : kNames) {
		if (name == entry.name) {
			*placement = entry.placement;
			return true;
		}
	}
	return false;
}

void Usage(const char* program)
{
	fprintf(stderr, "usage: %s [--placement top-left|top-right|bottom-left|bottom-right|top|bottom]\n", program);
}

}	// namespace

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--placement") == 0 && i + 1 < argc) {
			if (!ParsePlacement(argv[++i], &gApp.placement)) {
				Usage(argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("hdeskbar %s\n", APP_VERSION);
			return 0;
		} else {
			Usage(argv[0]);
			return 1;
		}
	}

	gApp.display = wl_display_connect(nullptr);
	if (gApp.display == nullptr) {
		fprintf(stderr, "hdeskbar: cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)\n");
		return 1;
	}
	gApp.registry = wl_display_get_registry(gApp.display);
	wl_registry_add_listener(gApp.registry, &kRegistryListener, nullptr);
	wl_display_roundtrip(gApp.display);
	if (gApp.compositor == nullptr || gApp.shm == nullptr || gApp.layerShell == nullptr) {
		fprintf(stderr, "hdeskbar: the compositor has no layer-shell (wlr-layer-shell) support\n");
		return 1;
	}

	gApp.surface = wl_compositor_create_surface(gApp.compositor);
	gApp.layerSurface = zwlr_layer_shell_v1_get_layer_surface(gApp.layerShell, gApp.surface, nullptr,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "hdeskbar");
	zwlr_layer_surface_v1_add_listener(gApp.layerSurface, &kLayerListener, nullptr);

	uint32_t anchor = 0;
	switch (gApp.placement) {
		case Placement::TopLeft: anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT; break;
		case Placement::TopRight: anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT; break;
		case Placement::BottomLeft: anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT; break;
		case Placement::BottomRight: anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT; break;
		case Placement::Top:
			anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			break;
		case Placement::Bottom:
			anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			break;
	}
	const bool vertical = IsVertical(gApp.placement);
	if (!vertical)
		gApp.height = 26;
	zwlr_layer_surface_v1_set_anchor(gApp.layerSurface, anchor);
	zwlr_layer_surface_v1_set_size(gApp.layerSurface, vertical ? gApp.width : 0, gApp.height);
	// (a horizontal bar keeps windows clear of itself; a vertical one is not a full edge, so it reserves nothing yet)
	zwlr_layer_surface_v1_set_exclusive_zone(gApp.layerSurface, vertical ? 0 : gApp.height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(gApp.layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(gApp.surface);

	while (gApp.running && wl_display_dispatch(gApp.display) != -1) {
	}
	return 0;
}
