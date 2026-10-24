/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The system tray: hDeskbar is a StatusNotifierHost (and, when nobody else owns org.kde.StatusNotifierWatcher, the watcher
 * too, queued for the name so it takes over if the other one goes away).
 */
#ifndef HDESKBAR_TRAY_H
#define HDESKBAR_TRAY_H

#include <gio/gio.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "icons.h"
#include "loop.h"
#include "util.h"

struct TrayItem {
    std::string service, path;
    std::string id, title, status;
    std::string iconName, attentionIconName, iconThemePath, menuPath;
    bool itemIsMenu = false;
    IconRef pixmap, attentionPixmap;
    guint signalSub = 0;
    guint nameWatch = 0;
    guint refreshTimer = 0;
    bool loaded = false;
    // Where the bar drew it, in the bar's pixels (for the pointer).
    double x = 0, y = 0, size = 0;
    uint64_t revision = 1;
};

static const char* kWatcherXml =
    "<node>"
    " <interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "  <method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    "  <signal name='StatusNotifierHostUnregistered'/>"
    " </interface>"
    "</node>";

class SystemTray {
public:
    std::function<void()> onChanged;

    void Init() {
        GError* err = nullptr;
        fBus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (fBus == nullptr) {
            WarnLog("no D-Bus session bus, system tray disabled: %s\n", err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
        fHostName = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
        g_bus_own_name_on_connection(fBus, fHostName.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);

        GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kWatcherXml, nullptr);
        static const GDBusInterfaceVTable vtable = {
            .method_call = &WatcherMethod,
            .get_property = &WatcherProperty,
            .set_property = nullptr,
            .padding = {nullptr},
        };
        fWatcherRegistration = g_dbus_connection_register_object(fBus, "/StatusNotifierWatcher",
            node->interfaces[0], &vtable, this, nullptr, nullptr);
        g_dbus_node_info_unref(node);

        g_bus_own_name_on_connection(fBus, "org.kde.StatusNotifierWatcher", G_BUS_NAME_OWNER_FLAGS_NONE,
            [](GDBusConnection*, const char*, gpointer p) {
                auto* self = static_cast<SystemTray*>(p);
                DebugLog("tray: we are the StatusNotifierWatcher\n");
                self->fIsWatcher = true;
                self->DropExternalWatcher();
                g_dbus_connection_emit_signal(self->fBus, nullptr, "/StatusNotifierWatcher",
                    "org.kde.StatusNotifierWatcher", "StatusNotifierHostRegistered", nullptr, nullptr);
            },
            [](GDBusConnection*, const char*, gpointer p) {
                auto* self = static_cast<SystemTray*>(p);
                DebugLog("tray: using an existing StatusNotifierWatcher\n");
                self->fIsWatcher = false;
                self->UseExternalWatcher();
            },
            this, nullptr);
    }

    const std::vector<std::unique_ptr<TrayItem>>& Items() const { return fItems; }

    std::vector<TrayItem*> VisibleItems() const {
        std::vector<TrayItem*> out;
        for (const auto& it : fItems) {
            if (it->loaded && it->status != "Passive") out.push_back(it.get());
        }
        return out;
    }

    void Activate(TrayItem* it, int x, int y) { CallItem(it, "Activate", g_variant_new("(ii)", x, y)); }
    void SecondaryActivate(TrayItem* it, int x, int y) { CallItem(it, "SecondaryActivate", g_variant_new("(ii)", x, y)); }
    void ContextMenu(TrayItem* it, int x, int y) { CallItem(it, "ContextMenu", g_variant_new("(ii)", x, y)); }
    void Scroll(TrayItem* it, int delta) { CallItem(it, "Scroll", g_variant_new("(is)", delta, "vertical")); }

    bool HasMenu(const TrayItem* it) const { return !it->menuPath.empty() && it->menuPath != "/"; }

private:
    // ---- watcher role ----
    static void WatcherMethod(GDBusConnection* conn, const char* sender, const char*, const char*,
        const char* method, GVariant* params, GDBusMethodInvocation* inv, gpointer p) {
        auto* self = static_cast<SystemTray*>(p);
        const char* arg = nullptr;
        g_variant_get(params, "(&s)", &arg);
        if (strcmp(method, "RegisterStatusNotifierItem") == 0) {
            std::string service, path;
            if (arg && arg[0] == '/') {
                service = sender;
                path = arg;
            } else {
                service = arg ? arg : sender;
                path = "/StatusNotifierItem";
            }
            std::string full = service + path;
            if (std::find(self->fRegistered.begin(), self->fRegistered.end(), full) == self->fRegistered.end()) {
                self->fRegistered.push_back(full);
                g_dbus_connection_emit_signal(conn, nullptr, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                    "StatusNotifierItemRegistered", g_variant_new("(s)", full.c_str()), nullptr);
            }
            self->AddItem(service, path);
        } else if (strcmp(method, "RegisterStatusNotifierHost") == 0) {
            g_dbus_connection_emit_signal(conn, nullptr, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                "StatusNotifierHostRegistered", nullptr, nullptr);
        }
        g_dbus_method_invocation_return_value(inv, nullptr);
    }

    static GVariant* WatcherProperty(GDBusConnection*, const char*, const char*, const char*,
        const char* prop, GError**, gpointer p) {
        auto* self = static_cast<SystemTray*>(p);
        if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
            for (const auto& s : self->fRegistered) g_variant_builder_add(&b, "s", s.c_str());
            return g_variant_builder_end(&b);
        }
        if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) return g_variant_new_boolean(TRUE);
        if (strcmp(prop, "ProtocolVersion") == 0) return g_variant_new_int32(0);
        return nullptr;
    }

    // ---- host role against somebody else's watcher ----
    void UseExternalWatcher() {
        DropExternalWatcher();
        g_dbus_connection_call(fBus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierHost", g_variant_new("(s)", fHostName.c_str()),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
        auto onSignal = [](GDBusConnection*, const char*, const char*, const char*, const char* signal,
            GVariant* params, gpointer p) {
            auto* self = static_cast<SystemTray*>(p);
            const char* s = nullptr;
            g_variant_get(params, "(&s)", &s);
            if (s == nullptr) return;
            if (strcmp(signal, "StatusNotifierItemRegistered") == 0) self->AddFromWatcherString(s);
        };
        fExternalSub = g_dbus_connection_signal_subscribe(fBus, "org.kde.StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher", nullptr, "/StatusNotifierWatcher", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, onSignal, this, nullptr);
        g_dbus_connection_call(fBus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "RegisteredStatusNotifierItems"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
            [](GObject*, GAsyncResult* res, gpointer p) {
                auto* self = static_cast<SystemTray*>(p);
                GVariant* reply = g_dbus_connection_call_finish(self->fBus, res, nullptr);
                if (reply == nullptr) return;
                GVariant* v = nullptr;
                g_variant_get(reply, "(v)", &v);
                if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("as"))) {
                    GVariantIter iter;
                    const char* s;
                    g_variant_iter_init(&iter, v);
                    while (g_variant_iter_next(&iter, "&s", &s)) self->AddFromWatcherString(s);
                }
                if (v) g_variant_unref(v);
                g_variant_unref(reply);
            }, this);
    }

    void DropExternalWatcher() {
        if (fExternalSub) g_dbus_connection_signal_unsubscribe(fBus, fExternalSub);
        fExternalSub = 0;
    }

    void AddFromWatcherString(const std::string& s) {
        size_t slash = s.find('/');
        if (slash == std::string::npos) AddItem(s, "/StatusNotifierItem");
        else AddItem(s.substr(0, slash), s.substr(slash));
    }

    // ---- items ----
    TrayItem* Find(const std::string& service, const std::string& path) {
        for (auto& it : fItems) if (it->service == service && it->path == path) return it.get();
        return nullptr;
    }

    void AddItem(const std::string& service, const std::string& path) {
        if (Find(service, path)) return;
        auto item = std::make_unique<TrayItem>();
        item->service = service;
        item->path = path;
        TrayItem* raw = item.get();
        fItems.push_back(std::move(item));
        DebugLog("tray: item %s%s\n", service.c_str(), path.c_str());

        raw->signalSub = g_dbus_connection_signal_subscribe(fBus, service.c_str(), "org.kde.StatusNotifierItem",
            nullptr, path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char* objPath, const char*, const char*, GVariant*, gpointer p) {
                auto* self = static_cast<SystemTray*>(p);
                for (auto& it : self->fItems) {
                    if (it->path == objPath) self->ScheduleRefresh(it.get());
                }
            }, this, nullptr);
        struct WatchCtx { SystemTray* tray; std::string service, path; };
        raw->nameWatch = g_bus_watch_name_on_connection(fBus, service.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE,
            nullptr,
            [](GDBusConnection*, const char*, gpointer p) {
                auto* ctx = static_cast<WatchCtx*>(p);
                ctx->tray->RemoveItem(ctx->service, ctx->path);
            },
            new WatchCtx{this, service, path}, [](gpointer p) { delete static_cast<WatchCtx*>(p); });
        Refresh(raw);
    }

    void RemoveItem(const std::string& service, const std::string& path) {
        std::string full = service + path;
        auto reg = std::find(fRegistered.begin(), fRegistered.end(), full);
        if (reg != fRegistered.end()) {
            fRegistered.erase(reg);
            if (fIsWatcher) {
                g_dbus_connection_emit_signal(fBus, nullptr, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                    "StatusNotifierItemUnregistered", g_variant_new("(s)", full.c_str()), nullptr);
            }
        }
        for (auto it = fItems.begin(); it != fItems.end(); ++it) {
            if ((*it)->service == service && (*it)->path == path) {
                TrayItem* raw = it->get();
                if (raw->signalSub) g_dbus_connection_signal_unsubscribe(fBus, raw->signalSub);
                if (raw->refreshTimer) g_source_remove(raw->refreshTimer);
                guint watch = raw->nameWatch;
                fItems.erase(it);
                // Unwatching from inside the vanished callback is fine but
                // must happen after we're done touching the item.
                if (watch) RunLater([watch]() { g_bus_unwatch_name(watch); });
                Changed();
                return;
            }
        }
    }

    void ScheduleRefresh(TrayItem* it) {
        if (it->refreshTimer) return;
        struct Ctx { SystemTray* tray; std::string service, path; };
        it->refreshTimer = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, [](gpointer p) -> gboolean {
            auto* ctx = static_cast<Ctx*>(p);
            if (TrayItem* item = ctx->tray->Find(ctx->service, ctx->path)) {
                item->refreshTimer = 0;
                ctx->tray->Refresh(item);
            }
            return G_SOURCE_REMOVE;
        }, new Ctx{this, it->service, it->path}, [](gpointer p) { delete static_cast<Ctx*>(p); });
    }

    void Refresh(TrayItem* it) {
        struct Ctx { SystemTray* tray; std::string service, path; };
        g_dbus_connection_call(fBus, it->service.c_str(), it->path.c_str(), "org.freedesktop.DBus.Properties",
            "GetAll", g_variant_new("(s)", "org.kde.StatusNotifierItem"), G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
            [](GObject*, GAsyncResult* res, gpointer p) {
                auto* ctx = static_cast<Ctx*>(p);
                GError* err = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(ctx->tray->fBus, res, &err);
                TrayItem* item = ctx->tray->Find(ctx->service, ctx->path);
                if (reply && item) {
                    GVariant* dict = g_variant_get_child_value(reply, 0);
                    ctx->tray->ApplyProperties(item, dict);
                    g_variant_unref(dict);
                } else if (err) {
                    DebugLog("tray: GetAll failed for %s: %s\n", ctx->service.c_str(), err->message);
                }
                g_clear_error(&err);
                if (reply) g_variant_unref(reply);
                delete ctx;
            }, new Ctx{this, it->service, it->path});
    }

    static IconRef DecodePixmap(GVariant* v) {
        // a(iiay), ARGB32 in network byte order; take the largest.
        if (v == nullptr || !g_variant_is_of_type(v, G_VARIANT_TYPE("a(iiay)"))) return nullptr;
        GVariantIter iter;
        g_variant_iter_init(&iter, v);
        int bestW = 0, bestH = 0;
        GVariant* bestData = nullptr;
        int w, h;
        GVariant* data;
        while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &data)) {
            if (w * h > bestW * bestH && static_cast<int>(g_variant_get_size(data)) >= w * h * 4) {
                if (bestData) g_variant_unref(bestData);
                bestData = data;
                bestW = w;
                bestH = h;
            } else {
                g_variant_unref(data);
            }
        }
        if (bestData == nullptr) return nullptr;
        const uint8_t* src = static_cast<const uint8_t*>(g_variant_get_data(bestData));
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bestW, bestH);
        unsigned char* dst = cairo_image_surface_get_data(s);
        int stride = cairo_image_surface_get_stride(s);
        for (int y = 0; y < bestH; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(dst + y * stride);
            for (int x = 0; x < bestW; ++x) {
                const uint8_t* px = src + (y * bestW + x) * 4;
                uint32_t a = px[0], r = px[1], g = px[2], b = px[3];
                r = r * a / 255; g = g * a / 255; b = b * a / 255;
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(s);
        g_variant_unref(bestData);
        return MakeIconRef(s);
    }

    void ApplyProperties(TrayItem* it, GVariant* dict) {
        auto str = [&](const char* key) -> std::string {
            GVariant* v = g_variant_lookup_value(dict, key, nullptr);
            std::string r;
            if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))) {
                r = g_variant_get_string(v, nullptr);
            }
            if (v) g_variant_unref(v);
            return r;
        };
        it->id = str("Id");
        it->title = str("Title");
        it->status = str("Status");
        it->iconName = str("IconName");
        it->attentionIconName = str("AttentionIconName");
        it->iconThemePath = str("IconThemePath");
        it->menuPath = str("Menu");
        GVariant* isMenu = g_variant_lookup_value(dict, "ItemIsMenu", G_VARIANT_TYPE_BOOLEAN);
        it->itemIsMenu = isMenu && g_variant_get_boolean(isMenu);
        if (isMenu) g_variant_unref(isMenu);
        GVariant* pix = g_variant_lookup_value(dict, "IconPixmap", nullptr);
        it->pixmap = DecodePixmap(pix);
        if (pix) g_variant_unref(pix);
        GVariant* apix = g_variant_lookup_value(dict, "AttentionIconPixmap", nullptr);
        it->attentionPixmap = DecodePixmap(apix);
        if (apix) g_variant_unref(apix);
        if (it->title.empty()) {
            // Tooltip title: (sa(iiay)ss) -- title is the third field.
            GVariant* tip = g_variant_lookup_value(dict, "ToolTip", nullptr);
            if (tip && g_variant_n_children(tip) >= 3) {
                GVariant* t = g_variant_get_child_value(tip, 2);
                if (g_variant_is_of_type(t, G_VARIANT_TYPE_STRING)) it->title = g_variant_get_string(t, nullptr);
                g_variant_unref(t);
            }
            if (tip) g_variant_unref(tip);
        }
        if (it->title.empty()) it->title = it->id;
        it->loaded = true;
        it->revision++;
        Changed();
    }

    void CallItem(TrayItem* it, const char* method, GVariant* params) {
        if (fBus == nullptr || it == nullptr) {
            if (params) g_variant_unref(g_variant_ref_sink(params));
            return;
        }
        g_dbus_connection_call(fBus, it->service.c_str(), it->path.c_str(), "org.kde.StatusNotifierItem", method,
            params, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
    }

    void Changed() {
        if (fChangePending) return;
        fChangePending = true;
        RunLater([this]() {
            fChangePending = false;
            if (onChanged) onChanged();
        });
    }

    GDBusConnection* fBus = nullptr;
    std::string fHostName;
    guint fWatcherRegistration = 0;
    guint fExternalSub = 0;
    bool fIsWatcher = false;
    std::vector<std::string> fRegistered;
    std::vector<std::unique_ptr<TrayItem>> fItems;
    bool fChangePending = false;
};

extern SystemTray gTray;

#endif
