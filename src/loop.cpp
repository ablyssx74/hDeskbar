/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "loop.h"

#include <wayland-client.h>

#include <cstring>

#include "util.h"

namespace {

struct WaylandSource {
	GSource base;
	gpointer tag;
	bool reading;
	wl_display* display;
	std::function<void()>* onLost;
};

GSourceFuncs kWaylandSourceFuncs = {
	// prepare
	[](GSource* s, gint* timeout) -> gboolean {
		auto* source = reinterpret_cast<WaylandSource*>(s);
		*timeout = -1;
		if (source->reading)
			return FALSE;
		while (wl_display_prepare_read(source->display) != 0) {
			if (wl_display_dispatch_pending(source->display) < 0)
				return TRUE;
		}
		wl_display_flush(source->display);
		source->reading = true;
		return FALSE;
	},
	// check
	[](GSource* s) -> gboolean {
		auto* source = reinterpret_cast<WaylandSource*>(s);
		GIOCondition condition = g_source_query_unix_fd(s, source->tag);
		if (source->reading) {
			if (condition & G_IO_IN) {
				if (wl_display_read_events(source->display) < 0) {
					source->reading = false;
					return TRUE;
				}
			} else
				wl_display_cancel_read(source->display);
			source->reading = false;
		}
		return (condition & (G_IO_IN | G_IO_ERR | G_IO_HUP)) != 0;
	},
	// dispatch
	[](GSource* s, GSourceFunc, gpointer) -> gboolean {
		auto* source = reinterpret_cast<WaylandSource*>(s);
		GIOCondition condition = g_source_query_unix_fd(s, source->tag);
		if (condition & (G_IO_ERR | G_IO_HUP)) {
			WarnLog("lost the Wayland connection\n");
			(*source->onLost)();
			return G_SOURCE_REMOVE;
		}
		if (wl_display_dispatch_pending(source->display) < 0) {
			WarnLog("Wayland protocol error: %s\n", strerror(wl_display_get_error(source->display)));
			(*source->onLost)();
			return G_SOURCE_REMOVE;
		}
		return G_SOURCE_CONTINUE;
	},
	nullptr, nullptr, nullptr,
};

}	// namespace

void AttachWaylandSource(wl_display* display, std::function<void()> onLost)
{
	GSource* s = g_source_new(&kWaylandSourceFuncs, sizeof(WaylandSource));
	auto* source = reinterpret_cast<WaylandSource*>(s);
	source->reading = false;
	source->display = display;
	source->onLost = new std::function<void()>(std::move(onLost));
	source->tag = g_source_add_unix_fd(s, wl_display_get_fd(display),
		static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP));
	g_source_set_priority(s, G_PRIORITY_HIGH);
	g_source_attach(s, nullptr);
	g_source_unref(s);
}

void RunLater(std::function<void()> fn)
{
	auto* heap = new std::function<void()>(std::move(fn));
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
		auto* f = static_cast<std::function<void()>*>(p);
		(*f)();
		delete f;
		return G_SOURCE_REMOVE;
	}, heap, nullptr);
}

guint RunAfter(unsigned ms, std::function<void()> fn)
{
	auto* heap = new std::function<void()>(std::move(fn));
	return g_timeout_add_full(G_PRIORITY_DEFAULT, ms, [](gpointer p) -> gboolean {
		(*static_cast<std::function<void()>*>(p))();
		return G_SOURCE_REMOVE;
	}, heap, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}
