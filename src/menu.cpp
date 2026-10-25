/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "menu.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "icons.h"
#include "loop.h"
#include "pill.h"
#include "scale.h"
#include "shm.h"
#include "style.h"
#include "util.h"
#include "wl.h"

namespace {

constexpr int kRow = 22;			// a row
constexpr int kSeparator = 7;		// a separator's
constexpr int kPad = 4;			// above the first row and under the last
constexpr double kMenuRadius = 10;		// the pill's round ends, and the radius of the trail's bends
constexpr int kOverlap = 20;			// a submenu opens this far over its parent, covering its arrows
constexpr int kIconSize = 16;
constexpr double kValueColumn = 54;		// the text of a bar row
constexpr double kBarWidth = 80;			// and its bar
constexpr double kMemBarWidth = 100;		// the bar of a memory row

struct Popup {
	std::vector<MenuItem> items;
	std::vector<int> tops;			// where each row starts, from the top of the content
	int content = 0;				// the height of all the rows
	int width = 0, height = 0;		// the size of the surface
	int scroll = 0;
	int hover = -1;
	bool hasIcons = false, hasChecks = false;
	int checkRoom = 0;			// a column of its own for the marks (none: they are in the icons' column)
	bool configured = false;
	Popup* parent = nullptr;
	int parentRow = -1;
	int openRow = -1;			// the row whose submenu is open: it stays lit
	int childSide = 0;			// where the open submenu is: 1 to the right of this menu, -1 to the left
	bool barAbove = false;		// the bar this menu starts from is below it: the trail starts at the bottom
	int trailSide = 0;			// the edge of this menu that faces its parent: -1 the left one, 1 the right one
	double trailRowY = 0;		// where the parent's row is, in this menu's pixels (the trail's bar starts there)
	bool mirror = false;			// opens to the left, chevrons on the left
	int lead = 0;				// the room of those chevrons
	int popupX = 0, popupY = 0;	// where the compositor put it, in its parent's pixels
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_popup* popup = nullptr;
	guint openTimer = 0;
	// a menu that is made again while it is open (the CPU graph's)
	std::function<std::vector<MenuItem>()> source;
	unsigned liveMs = 0;
	guint liveTimer = 0;
	double nameColumn = 0, numbersWidth = 0;	// the columns of the bar rows
};

std::vector<std::unique_ptr<Popup>> gStack;		// the menus that are open: the first is the root, the last the newest
Popup* gOver = nullptr;						// the menu the pointer is over
double gPointerX = 0, gPointerY = 0;

double MeasureLabel(const std::string& text)
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

// Where the rows are (and, with a bar, the room for it): the part of the layout that follows what the items are.
void Reflow(Popup* p)
{
	p->tops.clear();
	int y = 0;
	for (const MenuItem& item : p->items) {
		p->tops.push_back(y);
		y += item.separator ? kSeparator : kRow;
	}
	p->content = y;
}

// A size in KiB the way Haiku writes it: "923.98 MiB", "1.58 GiB".
std::string FormatSize(long long kb)
{
	double value = static_cast<double>(kb) * 1024.0;
	static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	int unit = 0;
	while (value >= 1024.0 && unit < 4) {
		value /= 1024.0;
		unit++;
	}
	char text[48];
	if (unit == 0)
		snprintf(text, sizeof(text), "%d B", static_cast<int>(value));
	else
		snprintf(text, sizeof(text), "%.2f %s", value, units[unit]);
	return text;
}

void LayOut(Popup* p)
{
	Reflow(p);
	double widest = 0;
	bool bars = false, memory = false, arrows = false;
	for (const MenuItem& item : p->items) {
		if (!item.separator)
			widest = std::max(widest, MeasureLabel(item.label));
		if (!item.iconName.empty() || item.icon)
			p->hasIcons = true;
		if (item.check != kCheckNone)
			p->hasChecks = true;
		if (item.barPercent >= 0)
			bars = true;
		if (item.memRow)
			memory = true;
		if (item.submenu && !item.noArrow)
			arrows = true;
	}
	// a menu with icons has the marks of its checked items where the icons are, unless an item has both
	bool both = false;
	for (const MenuItem& item : p->items)
		both |= item.check != kCheckNone && (!item.iconName.empty() || item.icon);
	p->checkRoom = (p->hasChecks && (!p->hasIcons || both)) ? 16 : 0;
	const double left = (p->hasIcons ? 34 : 12) + p->checkRoom;
	if (memory) {
		p->numbersWidth = 2 * MeasureLabel("1023.99 MiB") + 20;
		p->nameColumn = std::clamp(widest + 16, 120.0, 280.0);
		p->width = static_cast<int>(std::ceil(left + p->nameColumn + p->numbersWidth + kMemBarWidth + 16));
	} else if (bars) {
		p->nameColumn = std::clamp(widest + 16, 120.0, 280.0);
		p->width = static_cast<int>(std::ceil(left + p->nameColumn + kValueColumn + kBarWidth + 16 + (arrows ? 14 : 0)));
	} else {
		p->width = static_cast<int>(std::ceil(widest)) + 8 + (p->hasIcons ? 24 : 4) + p->checkRoom + 26;
		p->width = std::clamp(p->width, 150, 420);
	}
	p->lead = (p->mirror && arrows) ? 14 : 0;
	p->width += p->lead;
	const int most = (gWl.screenHeight > 0 ? gWl.screenHeight : 700) - 40;
	p->height = std::min(p->content + 2 * kPad, std::max(most, 100));
}

int ItemAt(const Popup* p, double y)
{
	const double inside = y - kPad + p->scroll;
	for (size_t i = 0; i < p->items.size(); i++) {
		const int top = p->tops[i];
		const int height = p->items[i].separator ? kSeparator : kRow;
		if (inside >= top && inside < top + height)
			return static_cast<int>(i);
	}
	return -1;
}

struct TrailBar {
	bool valid = false, onRight = false;
	float top = 0, bottom = 0;
};

// The menu as one pill, with the lit row and the snake trail on it, as hDesktop's Tracker draws them: the pill is the panel color,
// the lit row and the trail are the accent itself, shaded like a raised round bar. A submenu opens over its parent, and the trail
// runs unbroken from the root along the open submenus: the row that opened a submenu runs flush to the shared edge, the submenu's
// own row meets it flush, and the submenu adds a bar down that edge to wherever its own lit row sits. Worked out per pixel.
void DrawPill(Popup* p, cairo_t* cr, std::vector<char>* litRow, TrailBar* bar)
{
	using namespace PillGfx;
	cairo_matrix_t matrix;
	cairo_get_matrix(cr, &matrix);
	const float S = static_cast<float>(matrix.xx);		// device pixels to a logical one
	const int W = std::max(1, static_cast<int>(std::lround(p->width * S))), H = std::max(1, static_cast<int>(std::lround(p->height * S)));
	const float w = static_cast<float>(p->width), h = static_cast<float>(p->height), kR = static_cast<float>(kMenuRadius), kBarW = kR;
	std::vector<Piece> pieces;
	std::vector<Fillet> fillets;
	const Popup* parent = p->parent;
	// (the first menu of a bar at the side of the screen starts at the top of the bar, the way the Deskbar's leaf menu does)
	const bool fromBar = parent == nullptr && p->trailSide != 0;
	const float barEdge = p->barAbove ? static_cast<float>(H) : 0.0f;
	const bool parentLink = fromBar || (parent != nullptr && p->trailSide != 0 && parent->openRow == p->parentRow
		&& parent->childSide != 0 && (parent->hover < 0 || parent->hover == parent->openRow));
	const bool parentOnLeft = p->trailSide < 0;
	const bool opened = p->openRow >= 0 && p->childSide != 0;
	const int own = opened ? p->openRow : p->hover;		// my row on the trail
	float pTop = 0, pBottom = 0;
	if (parentLink && !fromBar) {
		pTop = static_cast<float>(p->trailRowY + 1);
		pBottom = static_cast<float>(p->trailRowY + kRow - 1);
	} else if (fromBar) {
		pTop = pBottom = barEdge;
	}
	auto cornerR = [&](bool flush, bool exposed) { return (!flush || exposed) ? kR : 0.0f; };
	auto addPiece = [&](float top, float bottom, bool childEdge, bool childOnRight, bool onTrail) {
		const bool cutTop = top < 0, cutBottom = bottom > h;
		const float t = std::max(top, 0.0f), b = std::min(bottom, h);
		if (b <= t)
			return;
		const bool leftFlush = (childEdge && !childOnRight) || (onTrail && parentOnLeft);
		const bool rightFlush = (childEdge && childOnRight) || (onTrail && !parentOnLeft);
		const float x0 = leftFlush ? 0.0f : 3.0f, x1 = rightFlush ? w : w - 3.0f;
		const bool expTop = onTrail && top < pTop - 0.5f, expBottom = onTrail && bottom > pBottom + 0.5f;
		pieces.push_back({ x0, t, x1 - x0, b - t,
			cutTop ? 0.0f : cornerR(leftFlush, onTrail && parentOnLeft && expTop),
			cutTop ? 0.0f : cornerR(rightFlush, onTrail && !parentOnLeft && expTop),
			cutBottom ? 0.0f : cornerR(rightFlush, onTrail && !parentOnLeft && expBottom),
			cutBottom ? 0.0f : cornerR(leftFlush, onTrail && parentOnLeft && expBottom) });
	};

	litRow->assign(p->items.size(), 0);
	float ownTop = 0, ownBottom = 0;
	bool hasOwn = false;
	for (size_t i = 0; i < p->items.size(); i++) {
		const MenuItem& item = p->items[i];
		if (item.separator)
			continue;
		const float y = static_cast<float>(kPad + p->tops[i] - p->scroll);
		const bool hovered = static_cast<int>(i) == p->hover && item.enabled && !item.header;
		const bool open = opened && static_cast<int>(i) == p->openRow && (p->hover < 0 || p->hover == p->openRow);
		const bool parentRow = parentLink && static_cast<int>(i) == own;
		if (parentRow) {
			ownTop = y + 1;
			ownBottom = y + kRow - 1;
			hasOwn = true;
		}
		if ((hovered || open || parentRow) && y + kRow > 0 && y < h) {
			(*litRow)[i] = 1;
			addPiece(y + 1, y + kRow - 1, open, p->childSide > 0, parentRow);
		}
	}
	*bar = TrailBar();
	if (parentLink) {
		// the bridge from my parent's row to mine
		float top = pTop, bottom = pBottom;
		if (hasOwn && ownBottom > ownTop) {
			top = std::min(top, ownTop);
			bottom = std::max(bottom, ownBottom);
		}
		top = std::max(top, 0.0f);
		bottom = std::min(bottom, h);
		if (bottom > top) {
			const bool extTop = top < pTop - 0.5f, extBottom = bottom > pBottom + 0.5f;
			// (a bar that starts at the edge of the menu, from the Deskbar, is straight there)
			const float outerT = extTop ? kR : 0.0f, innerT = (extTop || (fromBar && top <= 0.5f)) ? 0.0f : kR;
			const float outerB = extBottom ? kR : 0.0f, innerB = (extBottom || (fromBar && bottom >= h - 0.5f)) ? 0.0f : kR;
			if (parentOnLeft)
				pieces.push_back({ 0, top, kBarW, bottom - top, outerT, innerT, innerB, outerB });
			else
				pieces.push_back({ w - kBarW, top, kBarW, bottom - top, innerT, outerT, outerB, innerB });
			bar->valid = true;
			bar->onRight = !parentOnLeft;
			bar->top = top;
			bar->bottom = bottom;
			if (hasOwn && ownBottom > ownTop) {
				const float ex = parentOnLeft ? kBarW : w - kBarW;
				const int dx = parentOnLeft ? 1 : -1;
				if (top < ownTop - 0.5f && !(fromBar && ownTop - top < kR))
					fillets.push_back({ ex, ownTop, dx, -1 });
				if (bottom > ownBottom + 0.5f && !(fromBar && bottom - ownBottom < kR))
					fillets.push_back({ ex, ownBottom, dx, 1 });
			}
		}
	}

	// the pill: the corner next to the parent is square, and so is the one at the bottom when this level ends within its parent's height
	const bool overParent = (parent != nullptr || fromBar) && p->trailSide != 0;
	const bool squareBottom = overParent && parent != nullptr && p->popupY + p->height <= parent->height;
	const double tlR = (overParent && parentOnLeft && !(fromBar && p->barAbove)) ? 0.0 : kMenuRadius, trR = (overParent && !parentOnLeft) ? 0.0 : kMenuRadius;
	const double blR = ((squareBottom || (fromBar && p->barAbove)) && parentOnLeft) ? 0.0 : kMenuRadius, brR = (squareBottom && !parentOnLeft) ? 0.0 : kMenuRadius;
	RoundedRectPath(cr, 0, 0, w, h, tlR, trR, brR, blR);
	cairo_set_source_rgb(cr, kPanel.r, kPanel.g, kPanel.b);
	cairo_fill(cr);
	if (pieces.empty() && fillets.empty())
		return;

	float fx0 = w, fy0 = h, fx1 = 0, fy1 = 0;
	for (const Piece& q : pieces) {
		fx0 = std::min(fx0, q.x);
		fy0 = std::min(fy0, q.y);
		fx1 = std::max(fx1, q.x + q.w);
		fy1 = std::max(fy1, q.y + q.h);
	}
	for (const Fillet& f : fillets) {
		fx0 = std::min(fx0, std::min(f.x, f.x + f.dx * kR));
		fy0 = std::min(fy0, std::min(f.y, f.y + f.dy * kR));
		fx1 = std::max(fx1, std::max(f.x, f.x + f.dx * kR));
		fy1 = std::max(fy1, std::max(f.y, f.y + f.dy * kR));
	}
	const int margin = 10;		// what the shading reaches
	const int bx0 = std::max(0, static_cast<int>(std::floor(fx0 * S)) - margin), by0 = std::max(0, static_cast<int>(std::floor(fy0 * S)) - margin);
	const int bx1 = std::min(W - 1, static_cast<int>(std::ceil(fx1 * S)) + margin), by1 = std::min(H - 1, static_cast<int>(std::ceil(fy1 * S)) + margin);
	if (bx1 < bx0 || by1 < by0)
		return;
	const int BW = bx1 - bx0 + 1, BH = by1 - by0 + 1;
	std::vector<float> lit(static_cast<size_t>(BW) * BH, 0.0f), shade;
	for (const Piece& q : pieces) {
		const int x0 = std::max(bx0, static_cast<int>(std::floor(q.x * S)) - 1), x1 = std::min(bx1, static_cast<int>(std::ceil((q.x + q.w) * S)) + 1);
		const int y0 = std::max(by0, static_cast<int>(std::floor(q.y * S)) - 1), y1 = std::min(by1, static_cast<int>(std::ceil((q.y + q.h) * S)) + 1);
		for (int yy = y0; yy <= y1; yy++) {
			for (int xx = x0; xx <= x1; xx++) {
				float& dst = lit[static_cast<size_t>(yy - by0) * BW + (xx - bx0)];
				dst = std::max(dst, RoundRectCoverage(xx + 0.5f, yy + 0.5f, q.x * S, q.y * S, q.w * S, q.h * S, q.tl * S, q.tr * S, q.br * S, q.bl * S));
			}
		}
	}
	for (const Fillet& f : fillets) {
		const int x0 = std::max(bx0, static_cast<int>(std::floor(std::min(f.x, f.x + f.dx * kR) * S)) - 1);
		const int x1 = std::min(bx1, static_cast<int>(std::ceil(std::max(f.x, f.x + f.dx * kR) * S)) + 1);
		const int y0 = std::max(by0, static_cast<int>(std::floor(std::min(f.y, f.y + f.dy * kR) * S)) - 1);
		const int y1 = std::min(by1, static_cast<int>(std::ceil(std::max(f.y, f.y + f.dy * kR) * S)) + 1);
		for (int yy = y0; yy <= y1; yy++) {
			for (int xx = x0; xx <= x1; xx++) {
				float& dst = lit[static_cast<size_t>(yy - by0) * BW + (xx - bx0)];
				dst = std::max(dst, FilletCoverage(xx + 0.5f, yy + 0.5f, f.x * S, f.y * S, f.dx, f.dy, kR * S));
			}
		}
	}
	// (the lit shapes stay inside the pill's corners)
	const int cornerSize = static_cast<int>(std::ceil(kR * S)) + 1;
	for (int y = 0; y < BH; y++) {
		const int py = by0 + y;
		if (py >= cornerSize && py < H - cornerSize)
			continue;
		for (int x = 0; x < BW; x++) {
			const int px = bx0 + x;
			if (px >= cornerSize && px < W - cornerSize)
				continue;
			lit[static_cast<size_t>(y) * BW + x] *= RoundRectCoverage(px + 0.5f, py + 0.5f, 0, 0, static_cast<float>(W), static_cast<float>(H),
				static_cast<float>(tlR) * S, static_cast<float>(trR) * S, static_cast<float>(brR) * S, static_cast<float>(blR) * S);
		}
	}
	RaisedShade(lit, BW, BH, S, shade);

	cairo_surface_t* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BW, BH);
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(image);
		return;
	}
	uint8_t* bits = cairo_image_surface_get_data(image);
	const int stride = cairo_image_surface_get_stride(image);
	for (int yy = 0; yy < BH; yy++) {
		uint32_t* row = reinterpret_cast<uint32_t*>(bits + yy * stride);
		for (int xx = 0; xx < BW; xx++) {
			const size_t k = static_cast<size_t>(yy) * BW + xx;
			const float a = lit[k];
			if (a <= 0.0f) {
				row[xx] = 0;
				continue;
			}
			float r = static_cast<float>(kAccent.r * 255), g = static_cast<float>(kAccent.g * 255), b = static_cast<float>(kAccent.b * 255);
			const float rs = shade[k];
			if (rs > 0.0f) {
				r += (255.0f - r) * rs;
				g += (255.0f - g) * rs;
				b += (255.0f - b) * rs;
			} else if (rs < 0.0f) {
				r *= 1.0f + rs;
				g *= 1.0f + rs;
				b *= 1.0f + rs;
			}
			auto channel = [&](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f) * a)); };
			row[xx] = (static_cast<uint32_t>(std::lround(a * 255.0f)) << 24) | (channel(r) << 16) | (channel(g) << 8) | channel(b);
		}
	}
	cairo_surface_mark_dirty(image);
	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_set_source_surface(cr, image, bx0, by0);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_surface_destroy(image);
}

void Paint(Popup* p)
{
	if (!p->configured || p->surface == nullptr)
		return;
	wl_buffer* buffer = NewShmBuffer(p->width, p->height, [p](cairo_t* cr) {
		const double w = p->width, h = p->height;
		std::vector<char> litRow;
		TrailBar bar;
		DrawPill(p, cr, &litRow, &bar);
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_clip(cr);
		cairo_save(cr);
		cairo_translate(cr, p->lead, 0);		// (the rows have moved over for the chevrons on the left)
		const double rw = w - p->lead;
		for (size_t i = 0; i < p->items.size(); i++) {
			const MenuItem& item = p->items[i];
			const double y = kPad + p->tops[i] - p->scroll;
			if (y + kRow < 0 || y > h)
				continue;
			if (item.separator) {
				cairo_set_source_rgba(cr, 0, 0, 0, 0.20);
				cairo_rectangle(cr, 8, y + 3, rw - 16, 1);
				cairo_fill(cr);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
				cairo_rectangle(cr, 8, y + 4, rw - 16, 1);
				cairo_fill(cr);
				continue;
			}
			const bool lit = litRow[i] != 0;
			const double alpha = item.enabled ? 1.0 : 0.45;
			const double checkWidth = p->checkRoom, markX = p->checkRoom ? 0 : 6;
			const bool on = item.check == kCheckOn || item.check == kRadioOn;
			if (item.check != kCheckNone) {
				// a mark for a checked item, a dot for the chosen one of a set; an unchecked item that shares the icons' column has the
				// place of the mark shown as a box
				if (!on && item.check == kCheckOff && !p->checkRoom) {
					RoundedRectPath(cr, 12.5, y + kRow / 2.0 - 5.5, 11, 11, 2.5, 2.5, 2.5, 2.5);
					if (lit)
						cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
					else
						cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, 0.45 * alpha);
					cairo_set_line_width(cr, 1);
					cairo_stroke(cr);
				}
				if (on) {
					if (lit)
						cairo_set_source_rgb(cr, 1, 1, 1);
					else
						cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
					if (item.check == kCheckOn) {
						cairo_set_line_width(cr, 1.8);
						cairo_move_to(cr, 8 + markX, y + kRow / 2.0);
						cairo_line_to(cr, 11 + markX, y + kRow / 2.0 + 3);
						cairo_line_to(cr, 16 + markX, y + kRow / 2.0 - 4);
						cairo_stroke(cr);
					} else {
						cairo_new_path(cr);
						cairo_arc(cr, 12 + markX, y + kRow / 2.0, 3, 0, 2 * M_PI);
						cairo_fill(cr);
					}
				}
			}
			IconRef icon = item.icon;
			if (!icon && !item.iconName.empty()) {
				// (the names of an icon may be several, with commas: the first one the theme has)
				size_t from = 0;
				while (!icon && from <= item.iconName.size()) {
					size_t comma = item.iconName.find(',', from);
					if (comma == std::string::npos)
						comma = item.iconName.size();
					icon = CachedIcon(item.iconName.substr(from, comma - from), kIconSize);
					from = comma + 1;
				}
			}
			if (icon) {
				{
					cairo_save(cr);
					cairo_translate(cr, 10 + checkWidth, y + (kRow - kIconSize) / 2.0);
					cairo_scale(cr, kIconSize / static_cast<double>(cairo_image_surface_get_width(icon.get())),
						kIconSize / static_cast<double>(cairo_image_surface_get_height(icon.get())));
					cairo_set_source_surface(cr, icon.get(), 0, 0);
					cairo_paint_with_alpha(cr, alpha);
					cairo_restore(cr);
				}
			}
			const double textX = (p->hasIcons ? 34 : 12) + checkWidth;
			// a line of text in the row, cut off with ... at the width
			auto text = [&](const std::string& s, double x, double width, bool right = false) {
				if (width <= 0)
					return;
				PangoLayout* layout = pango_cairo_create_layout(cr);
				PangoFontDescription* font = pango_font_description_from_string(item.header ? "Sans Bold 9" : "Sans 9");
				pango_layout_set_font_description(layout, font);
				pango_layout_set_text(layout, s.c_str(), -1);
				pango_layout_set_width(layout, static_cast<int>(width * PANGO_SCALE));
				pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
				int textWidth = 0, textHeight = 0;
				pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
				if (lit)
					cairo_set_source_rgb(cr, 1, 1, 1);
				else
					cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, item.header ? 0.65 : alpha);
				cairo_move_to(cr, right ? x + width - textWidth : x, y + (kRow - textHeight) / 2.0);
				pango_cairo_show_layout(cr, layout);
				pango_font_description_free(font);
				g_object_unref(layout);
			};

			if (item.memRow) {
				// the name, the two sizes, and a bar in three pieces (what is written to, what else the program has, the rest)
				text(item.label, textX, p->nameColumn - 8);
				const double numbersX = textX + p->nameColumn, barX = numbersX + p->numbersWidth;
				text(FormatSize(item.memFirstKb), numbersX, p->numbersWidth / 2 - 4, true);
				text(FormatSize(item.memSecondKb), numbersX + p->numbersWidth / 2, p->numbersWidth / 2 - 4, true);
				const double barH = 10, barY = y + (kRow - barH) / 2, inner = kMemBarWidth - 2;
				const double e1 = std::clamp(item.memBarFirst, 0.0, 1.0) * inner, e2 = std::clamp(item.memBarSecond, 0.0, 1.0) * inner;
				cairo_set_source_rgb(cr, 1, 1, 1);
				cairo_rectangle(cr, barX + 1, barY + 1, inner, barH - 2);
				cairo_fill(cr);
				if (e2 > e1) {
					if (item.memSystem)
						cairo_set_source_rgb(cr, 193 / 255.0, 122 / 255.0, 1.0);
					else
						cairo_set_source_rgb(cr, 165 / 255.0, 165 / 255.0, 246 / 255.0);
					cairo_rectangle(cr, barX + 1 + e1, barY + 1, e2 - e1, barH - 2);
					cairo_fill(cr);
				}
				if (e1 > 0) {
					cairo_set_source_rgb(cr, 20 / 255.0, 20 / 255.0, 231 / 255.0);
					cairo_rectangle(cr, barX + 1, barY + 1, e1, barH - 2);
					cairo_fill(cr);
				}
				cairo_set_source_rgb(cr, 90 / 255.0, 90 / 255.0, 90 / 255.0);
				cairo_set_line_width(cr, 1);
				cairo_rectangle(cr, barX + 0.5, barY + 0.5, kMemBarWidth - 1, barH - 1);
				cairo_stroke(cr);
			} else if (item.barPercent >= 0) {
				// the name, the percentage, and a bar: green when the program takes a lot or little, orange between
				text(item.label, textX, p->nameColumn - 8);
				const double valueX = textX + p->nameColumn;
				text(item.valueText, valueX, kValueColumn - 4);
				const double barX = valueX + kValueColumn, barH = 10, barY = y + (kRow - barH) / 2;
				const double percent = std::clamp(item.barPercent, 0.0, 100.0);
				cairo_set_source_rgb(cr, 45 / 255.0, 45 / 255.0, 45 / 255.0);
				cairo_rectangle(cr, barX, barY, kBarWidth, barH);
				cairo_fill(cr);
				if (item.barPercent > 75.0)
					cairo_set_source_rgb(cr, 50 / 255.0, 205 / 255.0, 50 / 255.0);
				else if (item.barPercent > 35.0)
					cairo_set_source_rgb(cr, 1.0, 140 / 255.0, 0);
				else
					cairo_set_source_rgb(cr, 50 / 255.0, 205 / 255.0, 50 / 255.0);
				if (percent > 0) {
					cairo_rectangle(cr, barX, barY, kBarWidth * percent / 100.0, barH);
					cairo_fill(cr);
				}
				cairo_set_source_rgb(cr, 90 / 255.0, 90 / 255.0, 90 / 255.0);
				cairo_set_line_width(cr, 1);
				cairo_rectangle(cr, barX + 0.5, barY + 0.5, kBarWidth - 1, barH - 1);
				cairo_stroke(cr);
			} else {
				text(item.label, textX, rw - textX - 20);
			}

			const bool underTrail = bar.valid && bar.onRight == !p->mirror && y + kRow / 2.0 >= bar.top && y + kRow / 2.0 <= bar.bottom;
			if (item.submenu && !item.noArrow && !underTrail) {
				const double cx = p->mirror ? 9.0 - p->lead : rw - 14, cy = y + kRow / 2.0, d = p->mirror ? -2 : 2;
				cairo_set_line_width(cr, 1.6);
				cairo_move_to(cr, cx - d, cy - 3.5);
				cairo_line_to(cr, cx + d, cy);
				cairo_line_to(cr, cx - d, cy + 3.5);
				if (lit)
					cairo_set_source_rgb(cr, 1, 1, 1);
				else
					cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, alpha);
				cairo_stroke(cr);
			}
		}
		cairo_restore(cr);
		// the arrows of a long menu that scrolls
		if (p->content + 2 * kPad > p->height) {
			cairo_set_source_rgba(cr, 0.10, 0.10, 0.16, 0.55);
			if (p->scroll > 0) {
				cairo_move_to(cr, w / 2 - 4, 8);
				cairo_line_to(cr, w / 2, 3);
				cairo_line_to(cr, w / 2 + 4, 8);
				cairo_fill(cr);
			}
			if (p->scroll + p->height - 2 * kPad < p->content) {
				cairo_move_to(cr, w / 2 - 4, h - 8);
				cairo_line_to(cr, w / 2, h - 3);
				cairo_line_to(cr, w / 2 + 4, h - 8);
				cairo_fill(cr);
			}
		}
		cairo_restore(cr);

	});
	if (buffer == nullptr)
		return;
	AttachBuffer(p->surface, buffer, p->width, p->height);
	wl_surface_commit(p->surface);
}

void CloseFrom(size_t index);

// A menu that is made again while it is open: the new rows go in the same window (the room it has stays as it is; rows past the
// end are reached by scrolling). While a submenu of it is open it is left alone, so the row that submenu belongs to does not move.
gboolean LiveTick(gpointer data)
{
	Popup* p = static_cast<Popup*>(data);
	size_t at = 0;
	while (at < gStack.size() && gStack[at].get() != p)
		at++;
	if (at == gStack.size())
		return G_SOURCE_REMOVE;
	if (at + 1 < gStack.size() || !p->source)
		return G_SOURCE_CONTINUE;
	std::vector<MenuItem> fresh = p->source();
	if (fresh.empty())
		return G_SOURCE_CONTINUE;
	p->items = std::move(fresh);
	Reflow(p);
	const int room = std::max(0, p->content + 2 * kPad - p->height);
	p->scroll = std::clamp(p->scroll, 0, room);
	if (gOver == p)
		p->hover = ItemAt(p, gPointerY);
	Paint(p);
	return G_SOURCE_CONTINUE;
}

const xdg_surface_listener kXdgSurfaceListener = {
	[](void* data, xdg_surface* xs, uint32_t serial) {
		auto* p = static_cast<Popup*>(data);
		xdg_surface_ack_configure(xs, serial);
		p->configured = true;
		Paint(p);
	},
};

// The compositor has put the popup somewhere: that tells which side of its parent it is on, and so where the trail runs.
void PopupPlaced(void* data, xdg_popup*, int32_t x, int32_t y, int32_t, int32_t)
{
	auto* p = static_cast<Popup*>(data);
	p->popupX = x;
	p->popupY = y;
	Popup* parent = p->parent;
	if (parent == nullptr || p->parentRow < 0 || p->parentRow >= static_cast<int>(parent->tops.size()))
		return;
	const int side = (x + p->width / 2.0 > parent->width / 2.0) ? 1 : -1;
	p->trailSide = -side;
	parent->childSide = side;
	parent->openRow = p->parentRow;
	p->trailRowY = kPad + parent->tops[p->parentRow] - parent->scroll - y;
	Paint(p);
	Paint(parent);
}

const xdg_popup_listener kPopupListener = {
	PopupPlaced,
	[](void*, xdg_popup*) { RunLater([]() { CloseMenus(); }); },		// the compositor took it away: a press outside
	[](void*, xdg_popup*, uint32_t) {},
};

// Creates the popup for p: attached to the bar (parent null) or to another menu, at the rectangle (x, y, w, h) of the parent.
void Create(Popup* p, Popup* parent, int x, int y, int w, int h, uint32_t anchor, uint32_t gravity, int offsetX, int offsetY,
	uint32_t constraints)
{
	p->surface = wl_compositor_create_surface(gWl.compositor);
	ScaleTrackSurface(p->surface);
	p->xdgSurface = xdg_wm_base_get_xdg_surface(gWl.wmBase, p->surface);
	xdg_surface_add_listener(p->xdgSurface, &kXdgSurfaceListener, p);
	xdg_positioner* positioner = xdg_wm_base_create_positioner(gWl.wmBase);
	xdg_positioner_set_size(positioner, p->width, p->height);
	xdg_positioner_set_anchor_rect(positioner, x, y, std::max(1, w), std::max(1, h));
	xdg_positioner_set_anchor(positioner, anchor);
	xdg_positioner_set_gravity(positioner, gravity);
	xdg_positioner_set_constraint_adjustment(positioner, constraints);
	if (offsetX || offsetY)
		xdg_positioner_set_offset(positioner, offsetX, offsetY);
	p->popup = xdg_surface_get_popup(p->xdgSurface, parent != nullptr ? parent->xdgSurface : nullptr, positioner);
	xdg_positioner_destroy(positioner);
	xdg_popup_add_listener(p->popup, &kPopupListener, p);
	if (parent == nullptr)
		zwlr_layer_surface_v1_get_popup(gWl.layerSurface, p->popup);
	if (gWl.serial != 0)
		xdg_popup_grab(p->popup, gWl.seat, gWl.serial);	// (without a press to name, as in a test, it is left ungrabbed)
	wl_surface_commit(p->surface);
}

void Destroy(Popup* p)
{
	if (p->openTimer != 0)
		g_source_remove(p->openTimer);
	if (p->liveTimer != 0)
		g_source_remove(p->liveTimer);
	if (p->popup != nullptr)
		xdg_popup_destroy(p->popup);
	if (p->xdgSurface != nullptr)
		xdg_surface_destroy(p->xdgSurface);
	if (p->surface != nullptr) {
		ScaleForgetSurface(p->surface);
		wl_surface_destroy(p->surface);
	}
	if (gOver == p)
		gOver = nullptr;
}

// Closes the menu at index and the ones opened from it (the newest first: a popup goes before its parent).
void CloseFrom(size_t index)
{
	while (gStack.size() > index) {
		Destroy(gStack.back().get());
		gStack.pop_back();
	}
	if (!gStack.empty() && index > 0) {
		Popup* top = gStack.back().get();
		top->openRow = -1;
		top->childSide = 0;
		Paint(top);
	}
}

void OpenSubmenu(Popup* parent, int row)
{
	// (it may be open already)
	size_t at = 0;
	while (at < gStack.size() && gStack[at].get() != parent)
		at++;
	if (at == gStack.size())
		return;
	if (at + 1 < gStack.size() && gStack[at + 1]->parentRow == row)
		return;
	CloseFrom(at + 1);
	const MenuItem& item = parent->items[row];
	if (!item.submenu)
		return;
	auto child = std::make_unique<Popup>();
	child->items = item.submenu();
	if (child->items.empty())
		return;
	child->parent = parent;
	child->mirror = parent->mirror;
	child->parentRow = row;
	if (item.liveMs > 0) {
		child->source = item.submenu;
		child->liveMs = item.liveMs;
	}
	LayOut(child.get());
	Popup* raw = child.get();
	gStack.push_back(std::move(child));
	if (raw->liveMs > 0)
		raw->liveTimer = g_timeout_add(raw->liveMs, LiveTick, raw);
	const int y = kPad + parent->tops[row] - parent->scroll;
	Create(raw, parent, kOverlap, y, parent->width - 2 * kOverlap, kRow,
		parent->mirror ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_TOP_RIGHT,
		parent->mirror ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT, 0, -kPad,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
}

}	// namespace

void OpenMenu(std::vector<MenuItem> items, const MenuAnchor& anchor)
{
	CloseMenus();
	DebugLog("open a menu of %d items\n", static_cast<int>(items.size()));
	if (items.empty() || gWl.wmBase == nullptr || gWl.layerSurface == nullptr)
		return;
	auto root = std::make_unique<Popup>();
	root->items = std::move(items);
	root->mirror = anchor.mirror;
	LayOut(root.get());
	Popup* raw = root.get();
	gStack.push_back(std::move(root));
	if (anchor.side != 0) {
		// beside the rectangle, level with its top (the way the Deskbar's menu opens from a bar at the side of the screen)
		const bool left = anchor.side < 0;
		raw->trailSide = -anchor.side;
		raw->barAbove = anchor.above;		// (a bar at the bottom of the screen: the menu goes up from its header, and the trail starts at the bottom)
		// (the menu is level with the header's top, or with its bottom when the bar is at the bottom of the screen)
		const uint32_t anchorEdge = anchor.above ? (left ? XDG_POSITIONER_ANCHOR_BOTTOM_LEFT : XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT)
			: (left ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_TOP_RIGHT);
		const uint32_t gravity = anchor.above ? (left ? XDG_POSITIONER_GRAVITY_TOP_LEFT : XDG_POSITIONER_GRAVITY_TOP_RIGHT)
			: (left ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
		Create(raw, nullptr, anchor.x, anchor.y, anchor.w, anchor.h, anchorEdge, gravity, -anchor.side * 5, 0,
			XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
		return;
	}
	if (anchor.fromHeader) {
		raw->trailSide = -1;		// the trail runs down the edge at the header's side, from the bar
		raw->barAbove = anchor.above;
	}
	// (a mirrored menu hangs from the right end of the rectangle and goes to the left)
	Create(raw, nullptr, anchor.x, anchor.y, anchor.w, anchor.h,
		anchor.mirror ? (anchor.above ? XDG_POSITIONER_ANCHOR_TOP_RIGHT : XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT)
			: (anchor.above ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_BOTTOM_LEFT),
		anchor.mirror ? (anchor.above ? XDG_POSITIONER_GRAVITY_TOP_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_LEFT)
			: (anchor.above ? XDG_POSITIONER_GRAVITY_TOP_RIGHT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT), 0,
		anchor.fromHeader ? (anchor.above ? 5 : -5) : 0,		// (over the bar's edge)
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
			| XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
}

void CloseMenus()
{
	CloseFrom(0);
}

bool MenusOpen()
{
	return !gStack.empty();
}

bool MenuOwnsSurface(wl_surface* surface)
{
	for (const auto& p : gStack) {
		if (p->surface == surface)
			return true;
	}
	return false;
}

void MenuPointerEnter(wl_surface* surface, double x, double y)
{
	gOver = nullptr;
	for (const auto& p : gStack) {
		if (p->surface == surface)
			gOver = p.get();
	}
	MenuPointerMotion(x, y);
}

void MenuPointerLeave()
{
	if (gOver != nullptr && gOver->hover >= 0) {
		gOver->hover = -1;
		Paint(gOver);
	}
	gOver = nullptr;
}

void MenuPointerMotion(double x, double y)
{
	gPointerX = x;
	gPointerY = y;
	Popup* p = gOver;
	if (p == nullptr)
		return;
	const int row = ItemAt(p, y);
	if (row == p->hover)
		return;
	p->hover = row;
	Paint(p);
	// a row with a submenu opens it after a moment (so a pointer on its way across does not open every one); any other row
	// closes the menus that were opened from this one
	if (p->openTimer != 0) {
		g_source_remove(p->openTimer);
		p->openTimer = 0;
	}
	size_t at = 0;
	while (at < gStack.size() && gStack[at].get() != p)
		at++;
	if (row >= 0 && p->items[row].submenu && p->items[row].enabled) {
		struct Ctx { Popup* p; int row; };
		p->openTimer = g_timeout_add_full(G_PRIORITY_DEFAULT, 140, [](gpointer d) -> gboolean {
			auto* ctx = static_cast<Ctx*>(d);
			ctx->p->openTimer = 0;
			for (const auto& q : gStack) {
				if (q.get() == ctx->p) {
					OpenSubmenu(ctx->p, ctx->row);
					break;
				}
			}
			return G_SOURCE_REMOVE;
		}, new Ctx{ p, row }, [](gpointer d) { delete static_cast<Ctx*>(d); });
	} else if (at + 1 < gStack.size())
		CloseFrom(at + 1);
}

void MenuPointerButton(uint32_t, uint32_t state)
{
	Popup* p = gOver;
	if (p == nullptr || state != WL_POINTER_BUTTON_STATE_RELEASED)
		return;
	const int row = ItemAt(p, gPointerY);
	if (row < 0 || !p->items[row].enabled || p->items[row].separator)
		return;
	if (p->items[row].submenu) {
		OpenSubmenu(p, row);
		return;
	}
	std::function<void()> action = p->items[row].action;
	// (the menus go first: the action may open a window, or open another menu)
	RunLater([action]() {
		CloseMenus();
		if (action)
			action();
	});
}

void MenuPointerAxis(double value)
{
	Popup* p = gOver;
	if (p == nullptr)
		return;
	const int room = p->content + 2 * kPad - p->height;
	if (room <= 0)
		return;
	p->scroll = std::clamp(p->scroll + static_cast<int>(value * 2), 0, room);
	Paint(p);
}

// For testing: opens the submenu of a row of the menu at level (0 is the root), and lights the row.
void MenuDebugOpen(int level, int row)
{
	if (level < 0 || level >= static_cast<int>(gStack.size()))
		return;
	Popup* p = gStack[level].get();
	if (row < 0 || row >= static_cast<int>(p->items.size()))
		return;
	p->hover = row;
	Paint(p);
	OpenSubmenu(p, row);
}
