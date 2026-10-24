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
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "apps.h"
#include "cpu.h"
#include "icon.h"
#include "icons.h"
#include "leaf.h"
#include "loop.h"
#include "menu.h"
#include "settings.h"
#include "style.h"
#include "toplevels.h"
#include "tray.h"
#include "util.h"
#include "volume.h"
#include "wl.h"
#include "xdg-shell-client-protocol.h"

WlGlobals gWl;
SystemTray gTray;
VolumeControl gVolume;
ToplevelManager gToplevels;
AppDatabase* gApps = nullptr;

namespace {

constexpr double kRadius = 7;
constexpr int kHeaderHeight = 24;		// the block with the icon (a vertical bar)
constexpr int kRowHeight = 24;			// the row of the clock, and later the CPU graph and the tray
constexpr int kHeaderWidth = 48;		// the same block in a horizontal bar
constexpr int kIconSize = 20;
constexpr int kGraphWidth = 56;
constexpr int kRefreshMs = 500;		// how often the CPU graph moves


struct App {
	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	zwlr_layer_shell_v1* layerShell = nullptr;
	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	wl_seat* seat = nullptr;
	wl_output* output = nullptr;
	int outputHeight = 0;
	org_kde_plasma_window_management* plasmaWindows = nullptr;
	zwlr_foreign_toplevel_manager_v1* wlrToplevels = nullptr;
	wl_pointer* pointer = nullptr;
	GMainLoop* loop = nullptr;
	int hoverTab = -1, hoverRow = -1;
	bool volumeDrag = false;
	bool volumeMute = false;	// the press began on the speaker
	bool clickFirst = false;
	std::string debugClick, debugSubmenus, debugSettings;
	Placement shownPlacement = Placement::TopRight;
	bool redrawQueued = false;
	double pointerX = -1, pointerY = -1;

	int width = 150, height = 2 + kHeaderHeight + kRowHeight;		// what the bar asks for (a horizontal bar's width is the screen's)
	bool configured = false;
	CpuSampler cpu;
	std::vector<float> cpuShown;	// the bars as drawn: the loads, smoothed
};

App gApp;

bool IsVertical(Placement p)
{
	return p != Placement::Top && p != Placement::Bottom;
}


struct Rect {
	double x = 0, y = 0, w = 0, h = 0;

	bool Contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};


// ---- the program tabs ----

// The windows of one program (the way the Haiku Deskbar groups them by team), in the order the programs first appeared.
struct Team {
	std::string key, name, iconName;
	std::vector<Toplevel*> windows;
	bool collapsed = false;
	Toplevel* latest = nullptr;		// the window that was in front last
	bool active = false;			// one of its windows is the active one
};

std::vector<std::string> gTeamOrder;
std::map<std::string, bool> gCollapsed;		// the programs whose windows are not listed under their tab

std::string Capitalized(std::string s)
{
	if (!s.empty())
		s[0] = static_cast<char>(g_ascii_toupper(s[0]));
	return s;
}

std::vector<Team> BuildTeams()
{
	std::vector<Team> teams;
	if (!gSettings.showTabs)
		return teams;
	std::map<std::string, size_t> at;
	for (Toplevel* window : gToplevels.Windows()) {
		AppEntry* app = gApps != nullptr ? gApps->Resolve(window->appId) : nullptr;
		const std::string key = app != nullptr ? app->id : ToLower(window->appId.empty() ? window->title : window->appId);
		if (at.find(key) == at.end()) {
			Team team;
			team.key = key;
			team.name = app != nullptr ? app->name : Capitalized(window->appId.empty() ? window->title : window->appId);
			team.iconName = !window->themedIcon.empty() ? window->themedIcon : (app != nullptr ? app->iconName : window->appId);
			team.collapsed = gCollapsed[key];
			at[key] = teams.size();
			teams.push_back(team);
		}
		Team& team = teams[at[key]];
		team.windows.push_back(window);
		if (window->activated)
			team.active = true;
		if (team.latest == nullptr || window->lastActivated > team.latest->lastActivated)
			team.latest = window;
	}
	// the order the programs first came in: the ones that are gone are forgotten
	std::vector<std::string> order;
	for (const std::string& key : gTeamOrder) {
		if (at.count(key))
			order.push_back(key);
	}
	for (const Team& team : teams) {
		if (std::find(order.begin(), order.end(), team.key) == order.end())
			order.push_back(team.key);
	}
	gTeamOrder = order;
	std::vector<Team> sorted;
	for (const std::string& key : order)
		sorted.push_back(teams[at[key]]);
	return sorted;
}

struct TabRect {
	Rect tab, chevron;
	size_t team;
};

struct RowRect {
	Rect row;
	size_t team;
	Toplevel* window;
};

struct BoxRect {
	Rect box;
	size_t team;
};

struct TrayCell {
	TrayItem* item;
	Rect cell;
};

// Where everything is: worked out from the size of the bar and from what is in the tray, for drawing and for the pointer.
struct Layout {
	int width = 0, height = 0;		// what the bar needs (a horizontal bar's width is the screen's)
	Rect header, graph, clock, volume;
	std::vector<TrayCell> cells;
	std::vector<Team> teams;
	std::vector<TabRect> tabs;
	std::vector<RowRect> rows;
	std::vector<BoxRect> boxes;
};

constexpr int kCell = 22;				// a tray icon's cell
constexpr int kTrayIcon = 16;
constexpr int kTabHeight = 24;			// a program's tab
constexpr int kWindowRow = 20;			// a window, under its program's tab
constexpr int kVolumeRow = 20;			// the row of the volume slider in a vertical bar
constexpr int kVolumeWidth = 84;		// and its width in a horizontal one
constexpr int kSpeakerWidth = 20;		// the speaker at the slider's left end: it mutes
constexpr int kHorizontalHeight = 26;
constexpr int kVerticalWidth = 150;

// The width of a line of text in the bar's font.
double TextWidth(const std::string& text)
{
	static cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	static cairo_t* cr = cairo_create(scratch);
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string("Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text.c_str(), -1);
	int width = 0, height = 0;
	pango_layout_get_pixel_size(layout, &width, &height);
	pango_font_description_free(font);
	g_object_unref(layout);
	return width;
}

Layout MakeLayout(int width, int height)
{
	Layout layout;
	const std::vector<TrayItem*> items = gSettings.showTray ? gTray.VisibleItems() : std::vector<TrayItem*>();
	if (IsVertical(gSettings.placement)) {
		layout.width = width;
		layout.header = { 1, 1, width - 2.0, kHeaderHeight };
		layout.clock = { 1, 1.0 + kHeaderHeight, width - 2.0, kRowHeight };
		layout.graph = { 7, 1.0 + kHeaderHeight + 3, kGraphWidth, kRowHeight - 6.0 };
		const int columns = std::max(1, (width - 10) / kCell);
		const int rows = (static_cast<int>(items.size()) + columns - 1) / columns;
		double top = 1.0 + kHeaderHeight + kRowHeight + 2;
		if (gSettings.showVolume) {
			layout.volume = { 5, top, width - 10.0, kVolumeRow - 2.0 };
			top += kVolumeRow;
		}
		for (size_t i = 0; i < items.size(); i++)
			layout.cells.push_back({ items[i], { 5.0 + (i % columns) * kCell, top + (i / columns) * kCell, kCell, kCell } });
		double y = 1.0 + kHeaderHeight + kRowHeight + (gSettings.showVolume ? 2 + kVolumeRow : 0)
			+ (rows > 0 ? 2 + rows * kCell + 2 : 0);
		// the tabs: a program's tab, and under it, when it is open, the dark box with its windows. At the edge of the screen
		// they run flush, and are round at the other end.
		layout.teams = BuildTeams();
		const bool flushRight = gSettings.placement == Placement::TopRight || gSettings.placement == Placement::BottomRight;
		const double tabX = flushRight ? 3 : 1, tabW = width - 4.0;
		y += 2;
		for (size_t t = 0; t < layout.teams.size(); t++) {
			const Team& team = layout.teams[t];
			const Rect tab = { tabX, y, tabW, kTabHeight };
			layout.tabs.push_back({ tab, { tab.x + tab.w - 22, y, 20, kTabHeight }, t });
			y += kTabHeight;
			if (!team.collapsed && !team.windows.empty()) {
				const double boxX = flushRight ? tabX + 5 : tabX, boxW = tabW - 5;
				const Rect box = { boxX, y - 6, boxW, 6.0 + team.windows.size() * kWindowRow + 4 };
				layout.boxes.push_back({ box, t });
				for (size_t w = 0; w < team.windows.size(); w++)
					layout.rows.push_back({ { box.x + 1, y + w * kWindowRow, box.w - 2, kWindowRow }, t, team.windows[w] });
				y += team.windows.size() * kWindowRow + 4;
			}
			y += 3;
		}
		layout.height = static_cast<int>(std::ceil(y)) + 1;
		if (gApp.outputHeight > 0)
			layout.height = std::min(layout.height, gApp.outputHeight);
	} else {
		layout.height = kHorizontalHeight;
		layout.width = width;
		layout.header = { 1, 1, kHeaderWidth, kHorizontalHeight - 2.0 };
		layout.clock = { width - 111.0, 1, 110, kHorizontalHeight - 2.0 };
		layout.graph = { layout.clock.x - kGraphWidth - 6, 4, kGraphWidth, kHorizontalHeight - 8.0 };
		// (the first icon is the leftmost, the last the one next to the graph)
		for (size_t i = 0; i < items.size(); i++) {
			layout.cells.push_back({ items[i], { layout.graph.x - 6.0 - (items.size() - i) * kCell,
				(kHorizontalHeight - kCell) / 2.0, kCell, kCell } });
		}
		double right = layout.cells.empty() ? layout.graph.x : layout.cells[0].cell.x;
		if (gSettings.showVolume) {
			layout.volume = { right - 8.0 - kVolumeWidth, (kHorizontalHeight - 18) / 2.0, kVolumeWidth, 18 };
			right = layout.volume.x;
		}
		// the tabs, side by side after the header, as far as there is room
		layout.teams = BuildTeams();
		double x = layout.header.x + layout.header.w + 8;
		for (size_t t = 0; t < layout.teams.size(); t++) {
			const double w = std::clamp(TextWidth(layout.teams[t].name) + 54, 70.0, 180.0);
			if (x + w > right - 8)
				break;
			layout.tabs.push_back({ { x, 2, w, kHorizontalHeight - 4.0 }, { x + w - 18, 2, 14, kHorizontalHeight - 4.0 }, t });
			x += w + 4;
		}
	}
	return layout;
}

// ---- drawing ----

// The block at the top of the bar (the Deskbar's leaf): a raised bar in the SnakeTracker color, with the icon in its middle.
void DrawHeader(cairo_t* cr, double x, double y, double w, double h, double tl, double tr, double br, double bl)
{
	// the soft shade under the block
	for (int i = 3; i >= 1; i--) {
		RoundedRectPath(cr, x, y + i, w, h, tl, tr, br, bl);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.07);
		cairo_fill(cr);
	}

	cairo_pattern_t* gradient = cairo_pattern_create_linear(0, y, 0, y + h);
	const Color light = Mix(kAccent, {1, 1, 1}, 0.30), dark = Mix(kAccent, {0, 0, 0}, 0.22);
	cairo_pattern_add_color_stop_rgb(gradient, 0, light.r, light.g, light.b);
	cairo_pattern_add_color_stop_rgb(gradient, 0.55, kAccent.r, kAccent.g, kAccent.b);
	cairo_pattern_add_color_stop_rgb(gradient, 1, dark.r, dark.g, dark.b);
	RoundedRectPath(cr, x, y, w, h, tl, tr, br, bl);
	cairo_set_source(cr, gradient);
	cairo_fill(cr);
	cairo_pattern_destroy(gradient);

	// a fine light line along the top edge
	RoundedRectPath(cr, x + 0.5, y + 0.5, w - 1, h - 1, std::max(0.0, tl - 0.5), std::max(0.0, tr - 0.5),
		std::max(0.0, br - 0.5), std::max(0.0, bl - 0.5));
	cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	DrawSnakeTrackerIcon(cr, x + std::floor((w - kIconSize) / 2), y + std::floor((h - kIconSize) / 2), kIconSize);
}

// The CPU graph, in the place of ProcessController: a dark box with one bar for each processor.
void DrawCpuGraph(cairo_t* cr, double x, double y, double w, double h)
{
	const std::vector<float>& loads = gApp.cpu.coreLoads;
	const size_t count = std::max<size_t>(loads.size(), 1);
	if (gApp.cpuShown.size() != count)
		gApp.cpuShown.assign(count, 0.0f);
	RoundedRectPath(cr, x, y, w, h, 3, 3, 3, 3);
	cairo_set_source_rgba(cr, 0.03, 0.03, 0.05, 0.95);
	cairo_fill(cr);

	const double spacing = 1.5;
	const double barWidth = (w - 2 - spacing * (count + 1)) / count;
	cairo_set_source_rgba(cr, 0.57, 0.12, 0.99, 0.90);
	for (size_t i = 0; i < count; i++) {
		const float target = i < loads.size() ? loads[i] : 0.0f;
		gApp.cpuShown[i] = gApp.cpuShown[i] * 0.6f + target * 0.4f;
		const double left = x + 1 + spacing + i * (barWidth + spacing);
		const double top = y + h - 1 - gApp.cpuShown[i] * (h - 2);
		cairo_rectangle(cr, left, top, barWidth, y + h - 1 - top);
	}
	cairo_fill(cr);
}

void DrawClock(cairo_t* cr, double x, double y, double w, double h)
{
	time_t now = time(nullptr);
	struct tm local;
	localtime_r(&now, &local);
	char text[32];
	strftime(text, sizeof(text), "%-I:%M %p", &local);

	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string("Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text, -1);
	int textWidth = 0, textHeight = 0;
	pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
	cairo_set_source_rgb(cr, kOutline.r, kOutline.g, kOutline.b);
	cairo_move_to(cr, x + w - textWidth - 8, y + (h - textHeight) / 2);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(font);
	g_object_unref(layout);
}

// The volume, as the Haiku Deskbar has it: a speaker (it mutes), and a slider with a sunken groove and a small raised knob.
void DrawVolume(cairo_t* cr, const Rect& r)
{
	const bool muted = gVolume.muted || !gVolume.ready;
	const double level = gVolume.level;

	// the speaker: a box, a cone, and waves (or a bar across it, when muted)
	const double sx = r.x + 3, sy = r.y + r.h / 2;
	cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, muted ? 0.55 : 0.85);
	cairo_move_to(cr, sx, sy - 2.5);
	cairo_line_to(cr, sx + 3, sy - 2.5);
	cairo_line_to(cr, sx + 7, sy - 6);
	cairo_line_to(cr, sx + 7, sy + 6);
	cairo_line_to(cr, sx + 3, sy + 2.5);
	cairo_line_to(cr, sx, sy + 2.5);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.2);
	if (muted) {
		cairo_move_to(cr, sx + 9, sy - 3.5);
		cairo_line_to(cr, sx + 14, sy + 3.5);
		cairo_move_to(cr, sx + 14, sy - 3.5);
		cairo_line_to(cr, sx + 9, sy + 3.5);
		cairo_stroke(cr);
	} else {
		cairo_arc(cr, sx + 7, sy, 4, -M_PI / 3, M_PI / 3);
		cairo_stroke(cr);
		if (level > 0.5) {
			cairo_arc(cr, sx + 7, sy, 7, -M_PI / 3, M_PI / 3);
			cairo_stroke(cr);
		}
	}

	// the groove, sunk into the panel, and the part of it that is filled
	const double left = r.x + kSpeakerWidth + 2, right = r.x + r.w - 5, mid = r.y + r.h / 2;
	const double grooveTop = mid - 2.5, grooveHeight = 5;
	RoundedRectPath(cr, left - 1, grooveTop + 1, right - left + 2, grooveHeight, 2.5, 2.5, 2.5, 2.5);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.65);	// the light edge under the groove
	cairo_fill(cr);
	RoundedRectPath(cr, left - 1, grooveTop, right - left + 2, grooveHeight, 2.5, 2.5, 2.5, 2.5);
	cairo_set_source_rgb(cr, 0.46, 0.47, 0.58);
	cairo_fill(cr);
	const double knobX = left + std::clamp(level, 0.0, 1.0) * (right - left);
	if (knobX > left) {
		cairo_save(cr);
		RoundedRectPath(cr, left - 1, grooveTop, knobX - left + 1, grooveHeight, 2.5, 2.5, 2.5, 2.5);
		cairo_clip(cr);
		const Color fill = muted ? Color{0.62, 0.62, 0.68} : kAccent;
		cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	// a dark line along the top of the groove, as it is sunk
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, left + 1.5, grooveTop + 0.5);
	cairo_line_to(cr, right - 1.5, grooveTop + 0.5);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
	cairo_stroke(cr);

	// the knob: a small raised button
	const double knobW = 8, knobH = 14;
	const double kx = knobX - knobW / 2, ky = mid - knobH / 2;
	RoundedRectPath(cr, kx, ky + 1, knobW, knobH, 3, 3, 3, 3);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
	cairo_fill(cr);
	cairo_pattern_t* gradient = cairo_pattern_create_linear(0, ky, 0, ky + knobH);
	cairo_pattern_add_color_stop_rgb(gradient, 0, 0.98, 0.98, 1.0);
	cairo_pattern_add_color_stop_rgb(gradient, 1, 0.78, 0.79, 0.88);
	RoundedRectPath(cr, kx, ky, knobW, knobH, 3, 3, 3, 3);
	cairo_set_source(cr, gradient);
	cairo_fill(cr);
	cairo_pattern_destroy(gradient);
	RoundedRectPath(cr, kx + 0.5, ky + 0.5, knobW - 1, knobH - 1, 2.5, 2.5, 2.5, 2.5);
	cairo_set_source_rgba(cr, kOutline.r, kOutline.g, kOutline.b, muted ? 0.45 : 0.75);
	cairo_stroke(cr);
}

// ---- drawing the tabs ----

// Text in one line, cut off with ... where it is too long, centered up and down in the height.
void DrawLabel(cairo_t* cr, const std::string& text, double x, double y, double w, double h, const Color& color, double alpha = 1)
{
	if (w <= 0)
		return;
	PangoLayout* layout = pango_cairo_create_layout(cr);
	PangoFontDescription* font = pango_font_description_from_string("Sans 9");
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text.c_str(), -1);
	pango_layout_set_width(layout, static_cast<int>(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	int textWidth = 0, textHeight = 0;
	pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
	cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
	cairo_move_to(cr, x, y + (h - textHeight) / 2);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(font);
	g_object_unref(layout);
}

// A picture of an icon, size pixels square (a missing one leaves the place empty).
void DrawIconAt(cairo_t* cr, const std::string& name, double x, double y, int size, double alpha = 1)
{
	IconRef icon = CachedIcon(name, size);
	if (!icon)
		icon = CachedIcon("application-x-executable", size);
	if (!icon)
		return;
	const double width = cairo_image_surface_get_width(icon.get()), height = cairo_image_surface_get_height(icon.get());
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size / width, size / height);
	cairo_set_source_surface(cr, icon.get(), 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

// A bar that is round at both ends, or at one only: flush says which end is straight (-1 left, 1 right, 0 none). It is a bar
// that runs on past the straight end, and the caller clips to the rectangle.
void BarShape(cairo_t* cr, const Rect& r, int flush)
{
	const double radius = std::min(r.h / 2, 12.0);
	double x = r.x, w = r.w;
	if (flush < 0) {
		x -= 2 * radius;
		w += 2 * radius;
	} else if (flush > 0)
		w += 2 * radius;
	RoundedRectPath(cr, x, r.y, w, r.h, radius, radius, radius, radius);
}

bool FlushRight()
{
	return gSettings.placement == Placement::TopRight || gSettings.placement == Placement::BottomRight;
}

// The program's tab: a raised bar in the SnakeTracker color that melts into the panel towards its straight end, with a shade
// under it, the program's icon and name, and (in a vertical bar) the chevron that opens and closes its windows.
void DrawTab(cairo_t* cr, const TabRect& tr, const Team& team, bool vertical, bool hover)
{
	const Rect& r = tr.tab;
	const int flush = vertical ? (FlushRight() ? 1 : -1) : 0;
	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);

	cairo_pattern_t* raised = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
	const Color light = Mix(kAccent, {1, 1, 1}, 0.24), dark = Mix(kAccent, {0, 0, 0}, 0.14);
	cairo_pattern_add_color_stop_rgb(raised, 0, light.r, light.g, light.b);
	cairo_pattern_add_color_stop_rgb(raised, 0.5, kAccent.r, kAccent.g, kAccent.b);
	cairo_pattern_add_color_stop_rgb(raised, 1, dark.r, dark.g, dark.b);
	BarShape(cr, r, flush);
	cairo_set_source(cr, raised);
	cairo_fill(cr);
	cairo_pattern_destroy(raised);

	// the left 30% keeps its color, then it melts into the panel's
	cairo_pattern_t* fade = cairo_pattern_create_linear(r.x + r.w * 0.3, 0, r.x + r.w, 0);
	cairo_pattern_add_color_stop_rgba(fade, 0, kPanel.r, kPanel.g, kPanel.b, 0);
	cairo_pattern_add_color_stop_rgba(fade, 1, kPanel.r, kPanel.g, kPanel.b, 0.6);
	BarShape(cr, r, flush);
	cairo_set_source(cr, fade);
	cairo_fill(cr);
	cairo_pattern_destroy(fade);

	if (team.active || hover) {
		BarShape(cr, { r.x + 1, r.y + 1, r.w - 2, r.h - 2 }, flush);
		cairo_set_source_rgba(cr, 1, 1, 1, team.active ? 0.20 : 0.10);
		cairo_fill(cr);
	}

	// a shade along the bottom
	cairo_pattern_t* shade = cairo_pattern_create_linear(0, r.y + r.h - 5, 0, r.y + r.h);
	cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, 0);
	cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0.28);
	BarShape(cr, r, flush);
	cairo_set_source(cr, shade);
	cairo_fill(cr);
	cairo_pattern_destroy(shade);
	cairo_restore(cr);

	const double leftEdge = flush < 0 ? r.x + 8 : r.x + 10;
	DrawIconAt(cr, team.iconName, leftEdge, r.y + (r.h - 16) / 2, 16);
	const double labelRight = vertical ? tr.chevron.x : r.x + r.w - 12;
	DrawLabel(cr, team.name, leftEdge + 22, r.y, labelRight - (leftEdge + 22), r.h, {1, 1, 1});

	if (vertical) {
		// the chevron, a darker shade of the tab's color there
		const Color under = Mix(Mix(kAccent, kPanel, 0.6), {0, 0, 0}, 0.34);
		const double cx = tr.chevron.x + tr.chevron.w / 2, cy = r.y + r.h / 2;
		cairo_set_source_rgb(cr, under.r, under.g, under.b);
		cairo_set_line_width(cr, 1.6);
		if (team.collapsed) {
			cairo_move_to(cr, cx - 2, cy - 3.5);
			cairo_line_to(cr, cx + 2, cy);
			cairo_line_to(cr, cx - 2, cy + 3.5);
		} else {
			cairo_move_to(cr, cx - 3.5, cy - 2);
			cairo_line_to(cr, cx, cy + 2);
			cairo_line_to(cr, cx + 3.5, cy - 2);
		}
		cairo_stroke(cr);
	}
}

// The dark box under a program's tab with its windows, and the windows in it.
void DrawBox(cairo_t* cr, const Layout& layout, const BoxRect& br)
{
	const Rect& r = br.box;
	const int flush = FlushRight() ? 1 : -1;
	const Color box = Mix(kAccent, {0, 0, 0}, 0.45);
	cairo_save(cr);
	cairo_rectangle(cr, r.x - 1, r.y, r.w + 2, r.h + 3);
	cairo_clip(cr);
	// a soft shadow under it: two fainter copies, a pixel and two pixels lower
	BarShape(cr, { r.x, r.y + 2, r.w, r.h }, flush);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
	cairo_fill(cr);
	BarShape(cr, { r.x, r.y + 1, r.w, r.h }, flush);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.19);
	cairo_fill(cr);
	{
		const double radius = 8;
		double x = r.x, w = r.w;
		if (flush < 0) { x -= 2 * radius; w += 2 * radius; } else w += 2 * radius;
		RoundedRectPath(cr, x, r.y, w, r.h, radius, radius, radius, radius);
		cairo_set_source_rgb(cr, box.r, box.g, box.b);
		cairo_fill(cr);
	}
	cairo_restore(cr);

	for (size_t i = 0; i < layout.rows.size(); i++) {
		const RowRect& row = layout.rows[i];
		if (row.team != br.team)
			continue;
		const bool hover = static_cast<int>(i) == gApp.hoverRow;
		const Toplevel* window = row.window;
		if (window->activated || hover) {
			cairo_save(cr);
			cairo_rectangle(cr, row.row.x, row.row.y, row.row.w, row.row.h);
			cairo_clip(cr);
			BarShape(cr, { row.row.x + (flush > 0 ? 4 : 0), row.row.y + 1, row.row.w - 4, row.row.h - 2 }, flush);
			cairo_set_source_rgba(cr, kAccent.r, kAccent.g, kAccent.b, window->activated ? 0.95 : 0.50);
			cairo_fill(cr);
			cairo_restore(cr);
		}
		const std::string& icon = layout.teams[row.team].iconName;
		const double left = row.row.x + (flush > 0 ? 10 : 8);
		DrawIconAt(cr, icon, left, row.row.y + (row.row.h - 14) / 2, 14, window->minimized ? 0.5 : 1);
		DrawLabel(cr, window->title, left + 20, row.row.y, row.row.x + row.row.w - 8 - (left + 20), row.row.h,
			{245 / 255.0, 243 / 255.0, 252 / 255.0}, window->minimized ? 0.55 : 1);
	}
}

// The tray's icons, each in its cell, the one under the pointer lit.
void DrawTray(cairo_t* cr, const Layout& layout)
{
	for (const TrayCell& cell : layout.cells) {
		TrayItem* item = cell.item;
		item->x = cell.cell.x;
		item->y = cell.cell.y;
		item->size = cell.cell.w;
		if (cell.cell.Contains(gApp.pointerX, gApp.pointerY)) {
			RoundedRectPath(cr, cell.cell.x, cell.cell.y, cell.cell.w, cell.cell.h, 4, 4, 4, 4);
			cairo_set_source_rgba(cr, kAccent.r, kAccent.g, kAccent.b, 0.30);
			cairo_fill(cr);
		}
		const bool attention = item->status == "NeedsAttention";
		IconRef icon = attention && item->attentionPixmap ? item->attentionPixmap : item->pixmap;
		if (!icon) {
			const std::string& name = attention && !item->attentionIconName.empty() ? item->attentionIconName : item->iconName;
			icon = CachedIcon(name, kTrayIcon, item->iconThemePath);
		}
		if (!icon)
			icon = CachedIcon("application-x-executable", kTrayIcon);
		if (!icon)
			continue;
		const double width = cairo_image_surface_get_width(icon.get()), height = cairo_image_surface_get_height(icon.get());
		cairo_save(cr);
		cairo_translate(cr, cell.cell.x + (cell.cell.w - kTrayIcon) / 2, cell.cell.y + (cell.cell.h - kTrayIcon) / 2);
		cairo_scale(cr, kTrayIcon / width, kTrayIcon / height);
		cairo_set_source_surface(cr, icon.get(), 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
		cairo_paint(cr);
		cairo_restore(cr);
	}
}

void Draw(cairo_t* cr, const Layout& layout)
{
	const int width = layout.width, height = layout.height;
	// a corner that is a corner of the screen stays square; any other is round
	double tl = kRadius, tr = kRadius, br = kRadius, bl = kRadius;
	switch (gSettings.placement) {
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

	const bool vertical = IsVertical(gSettings.placement);
	if (vertical)
		DrawHeader(cr, layout.header.x, layout.header.y, layout.header.w, layout.header.h, tl > 0 ? tl - 1 : 0,
			tr > 0 ? tr - 1 : 0, 8, 8);
	else
		DrawHeader(cr, layout.header.x, layout.header.y, layout.header.w, layout.header.h, tl > 0 ? tl - 1 : 0, 8, 8,
			bl > 0 ? bl - 1 : 0);
	DrawClock(cr, layout.clock.x, layout.clock.y, layout.clock.w, layout.clock.h);
	if (gSettings.showCpu)
		DrawCpuGraph(cr, layout.graph.x, layout.graph.y, layout.graph.w, layout.graph.h);
	if (gSettings.showVolume)
		DrawVolume(cr, layout.volume);
	DrawTray(cr, layout);
	for (const BoxRect& box : layout.boxes)
		DrawBox(cr, layout, box);
	for (size_t i = 0; i < layout.tabs.size(); i++)
		DrawTab(cr, layout.tabs[i], layout.teams[layout.tabs[i].team], vertical, static_cast<int>(i) == gApp.hoverTab);
}

// ---- the buffer ----

void BufferRelease(void*, wl_buffer* buffer)
{
	wl_buffer_destroy(buffer);
}

const wl_buffer_listener kBufferListener = { BufferRelease };

wl_buffer* MakeBuffer(const Layout& layout)
{
	const int width = layout.width, height = layout.height;
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
	Draw(cr, layout);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);

	wl_shm_pool* pool = wl_shm_create_pool(gApp.shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_buffer_add_listener(buffer, &kBufferListener, nullptr);
	munmap(data, size);
	close(fd);
	return buffer;
}

Layout gLayout;

// Draws the bar. A vertical bar whose content has grown asks for another height first (and is drawn when the compositor has
// answered); a horizontal one has the width of the screen.
void Present()
{
	Layout layout = MakeLayout(gApp.width, gApp.height);
	if (IsVertical(gSettings.placement) && layout.height != gApp.height) {
		gApp.height = layout.height;
		zwlr_layer_surface_v1_set_size(gApp.layerSurface, gApp.width, gApp.height);
		wl_surface_commit(gApp.surface);
		return;
	}
	layout.width = gApp.width;
	gLayout = layout;
	wl_buffer* buffer = MakeBuffer(layout);
	if (buffer == nullptr)
		return;
	wl_surface_attach(gApp.surface, buffer, 0, 0);
	wl_surface_damage_buffer(gApp.surface, 0, 0, layout.width, layout.height);
	wl_surface_commit(gApp.surface);
}

// Draw again soon (once, however many things have changed).
void Redraw()
{
	if (gApp.redrawQueued || !gApp.configured)
		return;
	gApp.redrawQueued = true;
	RunLater([]() {
		gApp.redrawQueued = false;
		Present();
	});
}

// ---- the pointer ----

TrayCell* CellAt(double x, double y)
{
	for (TrayCell& cell : gLayout.cells) {
		if (cell.cell.Contains(x, y))
			return &cell;
	}
	return nullptr;
}

int TabAt(double x, double y)
{
	for (size_t i = 0; i < gLayout.tabs.size(); i++) {
		if (gLayout.tabs[i].tab.Contains(x, y))
			return static_cast<int>(i);
	}
	return -1;
}

int RowAt(double x, double y)
{
	for (size_t i = 0; i < gLayout.rows.size(); i++) {
		if (gLayout.rows[i].row.Contains(x, y))
			return static_cast<int>(i);
	}
	return -1;
}

void PointerEnter(void*, wl_pointer*, uint32_t, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
{
	gWl.pointerSurface = surface;
	if (surface != gApp.surface) {
		if (MenuOwnsSurface(surface))
			MenuPointerEnter(surface, wl_fixed_to_double(x), wl_fixed_to_double(y));
		else if (SettingsOwnsSurface(surface))
			SettingsPointerEnter(wl_fixed_to_double(x), wl_fixed_to_double(y));
		return;
	}
	gApp.pointerX = wl_fixed_to_double(x);
	gApp.pointerY = wl_fixed_to_double(y);
	Redraw();
}

void PointerLeave(void*, wl_pointer*, uint32_t, wl_surface* surface)
{
	if (surface != gApp.surface) {
		if (SettingsOwnsSurface(surface))
			SettingsPointerLeave();
		else
			MenuPointerLeave();
		if (gWl.pointerSurface == surface)
			gWl.pointerSurface = nullptr;
		return;
	}
	gApp.pointerX = gApp.pointerY = -1;
	gApp.hoverTab = gApp.hoverRow = -1;
	Redraw();
}

// The level the pointer's place on the slider stands for.
double VolumeAt(double x)
{
	const double left = gLayout.volume.x + kSpeakerWidth + 2, right = gLayout.volume.x + gLayout.volume.w - 5;
	return std::clamp((x - left) / (right - left), 0.0, 1.0);
}

void PointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
	if (gWl.pointerSurface != gApp.surface) {
		if (SettingsOwnsSurface(gWl.pointerSurface))
			SettingsPointerMotion(wl_fixed_to_double(x), wl_fixed_to_double(y));
		else
			MenuPointerMotion(wl_fixed_to_double(x), wl_fixed_to_double(y));
		return;
	}
	const double oldX = gApp.pointerX, oldY = gApp.pointerY;
	gApp.pointerX = wl_fixed_to_double(x);
	gApp.pointerY = wl_fixed_to_double(y);
	if (gApp.volumeDrag) {
		gVolume.SetLevel(static_cast<float>(VolumeAt(gApp.pointerX)));
		return;
	}
	// (only a move from one thing to another needs a new picture)
	const int tab = TabAt(gApp.pointerX, gApp.pointerY), row = RowAt(gApp.pointerX, gApp.pointerY);
	if (CellAt(oldX, oldY) != CellAt(gApp.pointerX, gApp.pointerY) || tab != gApp.hoverTab || row != gApp.hoverRow) {
		gApp.hoverTab = tab;
		gApp.hoverRow = row;
		Redraw();
	}
}

// A click on a tray icon.
void ClickCell(TrayCell* cell, uint32_t button)
{
	const int x = static_cast<int>(cell->cell.x), y = static_cast<int>(cell->cell.y);
	if (button == BTN_LEFT)
		gTray.Activate(cell->item, x, y);
	else if (button == BTN_RIGHT)
		gTray.ContextMenu(cell->item, x, y);
	else if (button == BTN_MIDDLE)
		gTray.SecondaryActivate(cell->item, x, y);
}

// The menu that the header opens.
void OpenLeafMenu()
{
	MenuAnchor anchor;
	anchor.x = static_cast<int>(gLayout.header.x);
	anchor.y = static_cast<int>(gLayout.header.y);
	anchor.w = static_cast<int>(gLayout.header.w);
	anchor.h = static_cast<int>(gLayout.header.h);
	anchor.above = gSettings.placement == Placement::Bottom || gSettings.placement == Placement::BottomLeft
		|| gSettings.placement == Placement::BottomRight;
	OpenMenu(LeafMenuItems(), anchor);
}

void PointerButton(void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
	if (serial != 0)
		gWl.serial = serial;
	if (gWl.pointerSurface != gApp.surface && gWl.pointerSurface != nullptr) {
		if (SettingsOwnsSurface(gWl.pointerSurface))
			SettingsPointerButton(button, state);
		else
			MenuPointerButton(button, state);
		return;
	}
	// the volume: a press on the slider sets the level and drags it; a click on the speaker mutes
	if (gSettings.showVolume && button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED && gLayout.volume.Contains(gApp.pointerX, gApp.pointerY)) {
			if (gApp.pointerX < gLayout.volume.x + kSpeakerWidth) {
				gApp.volumeMute = true;
			} else {
				gApp.volumeDrag = true;
				gVolume.SetLevel(static_cast<float>(VolumeAt(gApp.pointerX)));
			}
			return;
		}
		if (state == WL_POINTER_BUTTON_STATE_RELEASED && (gApp.volumeDrag || gApp.volumeMute)) {
			if (gApp.volumeMute && gApp.pointerX < gLayout.volume.x + kSpeakerWidth
				&& gLayout.volume.Contains(gApp.pointerX, gApp.pointerY))
				gVolume.SetMuted(!gVolume.muted);
			gApp.volumeDrag = gApp.volumeMute = false;
			return;
		}
	}
	if (state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	TrayCell* cell = CellAt(gApp.pointerX, gApp.pointerY);
	if (cell != nullptr) {
		ClickCell(cell, button);
		return;
	}
	if (button != BTN_LEFT)
		return;
	if (gLayout.header.Contains(gApp.pointerX, gApp.pointerY)) {
		OpenLeafMenu();
		return;
	}
	const int row = RowAt(gApp.pointerX, gApp.pointerY);
	if (row >= 0) {
		// a window of the program: bring it to the front
		gToplevels.Activate(gLayout.rows[row].window);
		return;
	}
	const int tab = TabAt(gApp.pointerX, gApp.pointerY);
	if (tab >= 0) {
		const TabRect& hit = gLayout.tabs[tab];
		const Team& team = gLayout.teams[hit.team];
		if (IsVertical(gSettings.placement) && hit.chevron.Contains(gApp.pointerX, gApp.pointerY)) {
			gCollapsed[team.key] = !team.collapsed;
			Redraw();
		} else if (team.latest != nullptr) {
			// the program's tab: its last window comes to the front; a lone window that is in front goes away
			if (team.windows.size() == 1 && team.latest->activated && !team.latest->minimized)
				gToplevels.Minimize(team.latest);
			else
				gToplevels.Activate(team.latest);
		}
	}
}

// The wheel over the slider changes the volume, two percent a notch.
void PointerAxis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value)
{
	if (gWl.pointerSurface != gApp.surface && gWl.pointerSurface != nullptr) {
		if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
			MenuPointerAxis(wl_fixed_to_double(value));
		return;
	}
	if (!gSettings.showVolume || axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !gLayout.volume.Contains(gApp.pointerX, gApp.pointerY))
		return;
	const double notches = wl_fixed_to_double(value) / 10.0;
	gVolume.SetLevel(gVolume.level - static_cast<float>(notches) * 0.02f);
}
void PointerFrame(void*, wl_pointer*) {}
void PointerAxisSource(void*, wl_pointer*, uint32_t) {}
void PointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void PointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}

const wl_pointer_listener kPointerListener = {
	PointerEnter, PointerLeave, PointerMotion, PointerButton, PointerAxis, PointerFrame, PointerAxisSource,
	PointerAxisStop, PointerAxisDiscrete, nullptr, nullptr, nullptr,
};

void SeatCapabilities(void*, wl_seat* seat, uint32_t capabilities)
{
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && gApp.pointer == nullptr) {
		gApp.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(gApp.pointer, &kPointerListener, nullptr);
	}
}

void SeatName(void*, wl_seat*, const char*) {}

const wl_seat_listener kSeatListener = { SeatCapabilities, SeatName };

// ---- layer surface ----

void LayerConfigure(void*, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
	if (width > 0)
		gApp.width = width;
	if (height > 0)
		gApp.height = height;
	gApp.configured = true;
	Present();
}

void LayerClosed(void*, zwlr_layer_surface_v1*)
{
	if (gApp.loop != nullptr)
		g_main_loop_quit(gApp.loop);
}

const zwlr_layer_surface_v1_listener kLayerListener = { LayerConfigure, LayerClosed };

// ---- registry ----

void WmBasePing(void*, xdg_wm_base* base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener kWmBaseListener = { WmBasePing };

// The height of the screen: a vertical bar is no taller than that.
void OutputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
void OutputMode(void*, wl_output*, uint32_t flags, int32_t, int32_t height, int32_t)
{
	if (flags & WL_OUTPUT_MODE_CURRENT)
		gApp.outputHeight = height;
}
void OutputDone(void*, wl_output*) {}
void OutputScale(void*, wl_output*, int32_t) {}

const wl_output_listener kOutputListener = { OutputGeometry, OutputMode, OutputDone, OutputScale, nullptr, nullptr };

void RegistryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		gApp.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		gApp.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		gWl.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 3u)));
		xdg_wm_base_add_listener(gWl.wmBase, &kWmBaseListener, nullptr);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && gApp.output == nullptr) {
		gApp.output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 2));
		wl_output_add_listener(gApp.output, &kOutputListener, nullptr);
	} else if (strcmp(interface, org_kde_plasma_window_management_interface.name) == 0 && version >= 17) {
		// (the protocol wants at least version 17)
		gApp.plasmaWindows = static_cast<org_kde_plasma_window_management*>(
			wl_registry_bind(registry, name, &org_kde_plasma_window_management_interface, 21));
	} else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		gApp.wlrToplevels = static_cast<zwlr_foreign_toplevel_manager_v1*>(
			wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 3));
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		gApp.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
		wl_seat_add_listener(gApp.seat, &kSeatListener, nullptr);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
		gApp.layerShell = static_cast<zwlr_layer_shell_v1*>(
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
}

void RegistryRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = { RegistryGlobal, RegistryRemove };

void Usage(const char* program)
{
	fprintf(stderr, "usage: %s [--placement top-left|top-right|bottom-left|bottom-right|top|bottom] [--no-cpu] [--no-tray] [--no-volume] [--no-tabs]\n", program);
}

// The bar's place on the screen: which edges it hangs on, its size, and the room it keeps clear. It is told to the compositor,
// and the bar is drawn again when the compositor has answered.
void ApplyPlacement()
{
	uint32_t anchor = 0;
	switch (gSettings.placement) {
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
	const bool vertical = IsVertical(gSettings.placement);
	if (vertical) {
		gApp.width = kVerticalWidth;
		gApp.height = 2 + kHeaderHeight + kRowHeight;
	} else
		gApp.height = kHorizontalHeight;
	gApp.shownPlacement = gSettings.placement;
	zwlr_layer_surface_v1_set_anchor(gApp.layerSurface, anchor);
	zwlr_layer_surface_v1_set_size(gApp.layerSurface, vertical ? gApp.width : 0, gApp.height);
	// (a horizontal bar keeps windows clear of itself; a vertical one is not a full edge, so it reserves nothing yet)
	zwlr_layer_surface_v1_set_exclusive_zone(gApp.layerSurface, vertical ? 0 : gApp.height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(gApp.layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(gApp.surface);
}

bool gTrayStarted = false, gVolumeStarted = false;

// What the settings window has changed: the place, the parts that are shown, the color.
void SettingsChanged()
{
	if (gSettings.placement != gApp.shownPlacement)
		ApplyPlacement();
	if (gSettings.showTray && !gTrayStarted) {
		gTrayStarted = true;
		gTray.onChanged = []() { Redraw(); };
		gTray.Init();
	}
	if (gSettings.showVolume && !gVolumeStarted) {
		gVolumeStarted = true;
		gVolume.onChanged = []() { Redraw(); };
		gVolume.Init();
	}
	Redraw();
}

}	// namespace

int main(int argc, char** argv)
{
	LoadSettings();
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--placement") == 0 && i + 1 < argc) {
			if (!ParsePlacement(argv[++i], &gSettings.placement)) {
				Usage(argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "--no-cpu") == 0) {
			gSettings.showCpu = false;
		} else if (strcmp(argv[i], "--no-tabs") == 0) {
			gSettings.showTabs = false;
		} else if (strcmp(argv[i], "--no-volume") == 0) {
			gSettings.showVolume = false;
		} else if (strcmp(argv[i], "--no-tray") == 0) {
			gSettings.showTray = false;
		} else if (strcmp(argv[i], "--click-first-tray-icon") == 0) {
			gApp.clickFirst = true;		// for testing: a left and a right click on the first icon, after a few seconds
		} else if (strcmp(argv[i], "--debug-click") == 0 && i + 1 < argc) {
			gApp.debugClick = argv[++i];		// for testing: tab:N, chevron:N or row:N, clicked a few seconds after the start
		} else if (strcmp(argv[i], "--debug-submenus") == 0 && i + 1 < argc) {
			gApp.debugSubmenus = argv[++i];		// for testing, after "--debug-click header": "2,5" opens row 2, then row 5 of that
		} else if (strcmp(argv[i], "--debug-settings") == 0 && i + 1 < argc) {
			gApp.debugSettings = argv[++i];		// for testing: opens the settings window, then clicks "x,y;x,y;..." in it
		} else if (strcmp(argv[i], "--debug") == 0) {
			gDebugEnabled = true;
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

	wl_display_roundtrip(gApp.display);		// (the seat and the output tell what they have)
	gApps = new AppDatabase();
	gToplevels.onChanged = []() { Redraw(); };
	gToplevels.Init(gApp.plasmaWindows, gApp.wlrToplevels, gApp.seat);
	if (!gToplevels.Available()) {
		WarnLog("no window-management protocol: the program tabs are left out.\n");
		if (DesktopIs("KDE"))
			WarnLog("On KDE Plasma, install hDeskbar (make install) so KWin gives it org_kde_plasma_window_management.\n");
	}
	gWl.display = gApp.display;
	gWl.compositor = gApp.compositor;
	gWl.shm = gApp.shm;
	gWl.seat = gApp.seat;
	gWl.screenHeight = gApp.outputHeight;
	gApp.surface = wl_compositor_create_surface(gApp.compositor);
	gWl.barSurface = gApp.surface;
	gApp.layerSurface = zwlr_layer_shell_v1_get_layer_surface(gApp.layerShell, gApp.surface, nullptr,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "hdeskbar");
	gWl.layerSurface = gApp.layerSurface;
	zwlr_layer_surface_v1_add_listener(gApp.layerSurface, &kLayerListener, nullptr);

	ApplyPlacement();

	// the clock changes on the minute, and the CPU graph moves twice a second
	gApp.loop = g_main_loop_new(nullptr, FALSE);
	AttachWaylandSource(gApp.display, []() { g_main_loop_quit(gApp.loop); });
	gOnSettingsChanged = SettingsChanged;
	if (gSettings.showTray) {
		gTrayStarted = true;
		gTray.onChanged = []() { Redraw(); };
		gTray.Init();
	}
	if (gSettings.showVolume) {
		gVolumeStarted = true;
		gVolume.onChanged = []() { Redraw(); };
		gVolume.Init();
	}
	gApp.cpu.Sample();
	static int shownMinute = -1;
	g_timeout_add(kRefreshMs, [](gpointer) -> gboolean {
		const int minute = static_cast<int>(time(nullptr) / 60);
		if (gSettings.showCpu) {
			gApp.cpu.Sample();
			Redraw();
		} else if (minute != shownMinute)
			Redraw();
		shownMinute = minute;
		return G_SOURCE_CONTINUE;
	}, nullptr);
	if (!gApp.debugClick.empty()) {
		RunAfter(5000, []() {
			const size_t colon = gApp.debugClick.find(':');
			const std::string what = gApp.debugClick.substr(0, colon);
			const size_t n = colon == std::string::npos ? 0 : strtoul(gApp.debugClick.c_str() + colon + 1, nullptr, 10);
			Rect r;
			if (what == "tab" && n < gLayout.tabs.size())
				r = gLayout.tabs[n].tab;
			else if (what == "chevron" && n < gLayout.tabs.size())
				r = gLayout.tabs[n].chevron;
			else if (what == "row" && n < gLayout.rows.size())
				r = gLayout.rows[n].row;
			else if (what == "header")
				r = gLayout.header;
			else {
				DebugLog("debug click: nothing like %s\n", gApp.debugClick.c_str());
				return;
			}
			gApp.pointerX = r.x + r.w / 2;
			gApp.pointerY = r.y + r.h / 2;
			DebugLog("debug click at %g,%g\n", gApp.pointerX, gApp.pointerY);
			PointerButton(nullptr, nullptr, 0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			PointerButton(nullptr, nullptr, 0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		});
	}
	if (!gApp.debugSubmenus.empty()) {
		RunAfter(7000, []() {
			int level = 0;
			const char* at = gApp.debugSubmenus.c_str();
			while (*at != '\0') {
				MenuDebugOpen(level++, atoi(at));
				const char* comma = strchr(at, ',');
				if (comma == nullptr)
					break;
				at = comma + 1;
			}
		});
	}
	if (!gApp.debugSettings.empty()) {
		RunAfter(4000, []() {
			OpenSettingsWindow();
			static size_t at = 0;
			g_timeout_add(1200, [](gpointer) -> gboolean {
				const std::string& list = gApp.debugSettings;
				if (at >= list.size())
					return G_SOURCE_REMOVE;
				const double x = atof(list.c_str() + at);
				const size_t comma = list.find(',', at);
				const double y = comma == std::string::npos ? 0 : atof(list.c_str() + comma + 1);
				const size_t semicolon = list.find(';', at);
				at = semicolon == std::string::npos ? list.size() : semicolon + 1;
				SettingsPointerEnter(x, y);
				SettingsPointerButton(BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
				return G_SOURCE_CONTINUE;
			}, nullptr);
		});
	}
	if (gApp.clickFirst) {
		RunAfter(4000, []() {
			if (!gLayout.cells.empty()) {
				ClickCell(&gLayout.cells[0], BTN_LEFT);
				ClickCell(&gLayout.cells[0], BTN_RIGHT);
			}
		});
	}
	g_main_loop_run(gApp.loop);
	return 0;
}
