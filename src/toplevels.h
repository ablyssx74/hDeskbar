/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The list of windows, from the compositor. Two protocols have it:
 *   * zwlr_foreign_toplevel_manager_v1: Hyprland, Sway, niri, labwc, Wayfire...
 *   * org_kde_plasma_window_management: KWin (which gives it only to a program installed with a .desktop file that asks for it)
 */
#ifndef HDESKBAR_TOPLEVELS_H
#define HDESKBAR_TOPLEVELS_H

#include <wayland-client.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "loop.h"
#include "plasma-window-management-client-protocol.h"
#include "util.h"
#define namespace namespace_
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#undef namespace

struct Toplevel {
    uint64_t uid = 0;
    std::string title;
    std::string appId;
    std::string themedIcon;     // KWin tells us the icon name directly
    std::string uuid;           // KWin window uuid (screencast target)
    bool activated = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    bool skipTaskbar = false;
    bool hasParent = false;     // transient dialogs are folded into their parent
    bool ready = false;
    bool onAllDesktops = false;
    uint32_t pid = 0;
    std::set<std::string> desktops;  // KWin virtual desktop ids
    int gx = 0, gy = 0, gw = 0, gh = 0;  // KWin: window geometry in compositor space
    bool hasGeometry = false;
    uint64_t lastActivated = 0;
    zwlr_foreign_toplevel_handle_v1* wlr = nullptr;
    org_kde_plasma_window* plasma = nullptr;
};

class ToplevelManager {
public:
    std::function<void()> onChanged;

    bool Available() const { return fBackend != kNone; }
    const char* BackendName() const {
        return fBackend == kPlasma ? "KDE plasma-window-management"
            : fBackend == kWlr ? "wlr-foreign-toplevel-management" : "none";
    }

    // plasma: KWin's window management; wlr: the wlr-foreign-toplevel manager of the other compositors (either may be null).
    // Called as each of the three is known: the compositor announces the windows that exist right when its protocol is bound, so
    // the listener must be on by then (events for a proxy without one are dropped). Any of the three may be null.
    void Init(org_kde_plasma_window_management* plasma, zwlr_foreign_toplevel_manager_v1* wlr, wl_seat* seat) {
        if (seat) fSeat = seat;
        if (plasma && fBackend == kNone) {
            fBackend = kPlasma;
            org_kde_plasma_window_management_add_listener(plasma, &kPlasmaMgmtListener, this);
        } else if (wlr && fBackend == kNone) {
            fBackend = kWlr;
            zwlr_foreign_toplevel_manager_v1_add_listener(wlr, &kWlrMgrListener, this);
        }
        DebugLog("taskbar backend: %s\n", BackendName());
    }

    // Windows eligible for the dock, in the order they first appeared.
    std::vector<Toplevel*> Windows() const {
        std::vector<Toplevel*> out;
        for (const auto& t : fWindows) {
            if (t->ready && !t->skipTaskbar && !t->hasParent) out.push_back(t.get());
        }
        return out;
    }

    void Activate(Toplevel* t) {
        if (t == nullptr) return;
        DebugLog("activate window '%s' (%s)\n", t->title.c_str(), t->appId.c_str());
        if (t->wlr && fSeat) {
            if (t->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(t->wlr);
            zwlr_foreign_toplevel_handle_v1_activate(t->wlr, fSeat);
        } else if (t->plasma) {
            org_kde_plasma_window_set_state(t->plasma,
                ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE | ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED,
                ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE);
        }
    }

    // KWin only: put a window on one virtual desktop (and take it off the others).
    bool CanMoveToWorkspace(const Toplevel* t) const { return t && t->plasma; }
    void MoveToWorkspace(Toplevel* t, const std::string& desktopId) {
        if (t == nullptr || t->plasma == nullptr || desktopId.empty()) return;
        if (t->onAllDesktops) {
            org_kde_plasma_window_set_state(t->plasma, ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ON_ALL_DESKTOPS, 0);
        }
        org_kde_plasma_window_request_enter_virtual_desktop(t->plasma, desktopId.c_str());
        std::vector<std::string> leave;
        for (const auto& id : t->desktops) if (id != desktopId) leave.push_back(id);
        for (const auto& id : leave) org_kde_plasma_window_request_leave_virtual_desktop(t->plasma, id.c_str());
    }

    void Unminimize(Toplevel* t) {
        if (t == nullptr) return;
        if (t->wlr) zwlr_foreign_toplevel_handle_v1_unset_minimized(t->wlr);
        else if (t->plasma) org_kde_plasma_window_set_state(t->plasma, ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED, 0);
    }

    void Minimize(Toplevel* t) {
        if (t == nullptr) return;
        if (t->wlr) zwlr_foreign_toplevel_handle_v1_set_minimized(t->wlr);
        else if (t->plasma) {
            org_kde_plasma_window_set_state(t->plasma, ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED,
                ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED);
        }
    }

    void Close(Toplevel* t) {
        if (t == nullptr) return;
        if (t->wlr) zwlr_foreign_toplevel_handle_v1_close(t->wlr);
        else if (t->plasma) org_kde_plasma_window_close(t->plasma);
    }

    // Tells the compositor where the window's dock icon is, so minimize
    // animations fly into it.
    void SetIconRect(Toplevel* t, wl_surface* dockSurface, int x, int y, int w, int h) {
        if (t == nullptr || dockSurface == nullptr || w <= 0 || h <= 0) return;
        if (t->wlr) zwlr_foreign_toplevel_handle_v1_set_rectangle(t->wlr, dockSurface, x, y, w, h);
        else if (t->plasma) {
            org_kde_plasma_window_set_minimized_geometry(t->plasma, dockSurface,
                std::max(0, x), std::max(0, y), w, h);
        }
    }

    bool IsOnDesktop(const Toplevel* t, const std::string& desktopId) const {
        if (fBackend != kPlasma || desktopId.empty()) return true;
        return t->onAllDesktops || t->desktops.empty() || t->desktops.count(desktopId) > 0;
    }

private:
    enum Backend { kNone, kWlr, kPlasma };

    Toplevel* NewWindow() {
        auto t = std::make_unique<Toplevel>();
        t->uid = ++fNextUid;
        Toplevel* raw = t.get();
        fWindows.push_back(std::move(t));
        return raw;
    }

    void Remove(Toplevel* t) {
        for (auto it = fWindows.begin(); it != fWindows.end(); ++it) {
            if (it->get() == t) {
                if (t->wlr) zwlr_foreign_toplevel_handle_v1_destroy(t->wlr);
                if (t->plasma) org_kde_plasma_window_destroy(t->plasma);
                fWindows.erase(it);
                break;
            }
        }
        Changed();
    }

    void Changed() {
        if (fChangePending) return;
        fChangePending = true;
        RunLater([this]() {
            fChangePending = false;
            if (onChanged) onChanged();
        });
    }

    Toplevel* FindWlr(zwlr_foreign_toplevel_handle_v1* h) {
        for (const auto& t : fWindows) if (t->wlr == h) return t.get();
        return nullptr;
    }

    static void SetActivated(Toplevel* t, bool active) {
        if (active && !t->activated) t->lastActivated = NowMs();
        t->activated = active;
    }

    static const zwlr_foreign_toplevel_manager_v1_listener kWlrMgrListener;
    static const zwlr_foreign_toplevel_handle_v1_listener kWlrHandleListener;
    static const org_kde_plasma_window_management_listener kPlasmaMgmtListener;
    static const org_kde_plasma_window_listener kPlasmaWindowListener;

    Backend fBackend = kNone;
    wl_seat* fSeat = nullptr;
    std::vector<std::unique_ptr<Toplevel>> fWindows;
    uint64_t fNextUid = 0;
    bool fChangePending = false;
};


struct WlrHandleCtx { ToplevelManager* mgr; Toplevel* t; };

inline const zwlr_foreign_toplevel_manager_v1_listener ToplevelManager::kWlrMgrListener = {
    .toplevel = [](void* data, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* h) {
        auto* self = static_cast<ToplevelManager*>(data);
        Toplevel* t = self->NewWindow();
        t->wlr = h;
        zwlr_foreign_toplevel_handle_v1_add_listener(h, &kWlrHandleListener, self);
    },
    .finished = [](void*, zwlr_foreign_toplevel_manager_v1*) {},
};

inline const zwlr_foreign_toplevel_handle_v1_listener ToplevelManager::kWlrHandleListener = {
    .title = [](void* data, zwlr_foreign_toplevel_handle_v1* h, const char* title) {
        if (Toplevel* t = static_cast<ToplevelManager*>(data)->FindWlr(h)) t->title = title ? title : "";
    },
    .app_id = [](void* data, zwlr_foreign_toplevel_handle_v1* h, const char* appId) {
        if (Toplevel* t = static_cast<ToplevelManager*>(data)->FindWlr(h)) t->appId = appId ? appId : "";
    },
    .output_enter = [](void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {},
    .output_leave = [](void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) {},
    .state = [](void* data, zwlr_foreign_toplevel_handle_v1* h, wl_array* state) {
        Toplevel* t = static_cast<ToplevelManager*>(data)->FindWlr(h);
        if (t == nullptr) return;
        bool active = false;
        t->minimized = t->maximized = t->fullscreen = false;
        const uint32_t* states = static_cast<const uint32_t*>(state->data);
        for (size_t i = 0; i < state->size / sizeof(uint32_t); ++i) {
            const uint32_t* s = &states[i];
            if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) active = true;
            else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) t->minimized = true;
            else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) t->maximized = true;
            else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) t->fullscreen = true;
        }
        SetActivated(t, active);
    },
    .done = [](void* data, zwlr_foreign_toplevel_handle_v1* h) {
        auto* self = static_cast<ToplevelManager*>(data);
        if (Toplevel* t = self->FindWlr(h)) {
            t->ready = true;
            self->Changed();
        }
    },
    .closed = [](void* data, zwlr_foreign_toplevel_handle_v1* h) {
        auto* self = static_cast<ToplevelManager*>(data);
        if (Toplevel* t = self->FindWlr(h)) self->Remove(t);
    },
    .parent = [](void* data, zwlr_foreign_toplevel_handle_v1* h, zwlr_foreign_toplevel_handle_v1* parent) {
        if (Toplevel* t = static_cast<ToplevelManager*>(data)->FindWlr(h)) t->hasParent = (parent != nullptr);
    },
};

inline const org_kde_plasma_window_management_listener ToplevelManager::kPlasmaMgmtListener = {
    .show_desktop_changed = [](void*, org_kde_plasma_window_management*, uint32_t) {},
    .window = [](void*, org_kde_plasma_window_management*, uint32_t) {},  // deprecated; window_with_uuid follows
    .stacking_order_changed = [](void*, org_kde_plasma_window_management*, wl_array*) {},
    .stacking_order_uuid_changed = [](void*, org_kde_plasma_window_management*, const char*) {},
    .window_with_uuid = [](void* data, org_kde_plasma_window_management* mgmt, uint32_t, const char* uuid) {
        auto* self = static_cast<ToplevelManager*>(data);
        Toplevel* t = self->NewWindow();
        t->uuid = uuid ? uuid : "";
        t->plasma = org_kde_plasma_window_management_get_window_by_uuid(mgmt, uuid);
        org_kde_plasma_window_add_listener(t->plasma, &kPlasmaWindowListener, new WlrHandleCtx{self, t});
    },
    .stacking_order_changed_2 = [](void*, org_kde_plasma_window_management*) {},
};

#define PLASMA_CTX(data) auto* ctx = static_cast<WlrHandleCtx*>(data); Toplevel* t = ctx->t; (void)t

inline const org_kde_plasma_window_listener ToplevelManager::kPlasmaWindowListener = {
    .title_changed = [](void* data, org_kde_plasma_window*, const char* title) {
        PLASMA_CTX(data);
        t->title = title ? title : "";
        if (t->ready) ctx->mgr->Changed();
    },
    .app_id_changed = [](void* data, org_kde_plasma_window*, const char* appId) {
        PLASMA_CTX(data);
        t->appId = appId ? appId : "";
        if (t->ready) ctx->mgr->Changed();
    },
    .state_changed = [](void* data, org_kde_plasma_window*, uint32_t flags) {
        PLASMA_CTX(data);
        SetActivated(t, flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE);
        t->minimized = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED;
        t->maximized = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MAXIMIZED;
        t->fullscreen = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_FULLSCREEN;
        t->skipTaskbar = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_SKIPTASKBAR;
        t->onAllDesktops = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ON_ALL_DESKTOPS;
        if (t->ready) ctx->mgr->Changed();
    },
    .virtual_desktop_changed = [](void*, org_kde_plasma_window*, int32_t) {},
    .themed_icon_name_changed = [](void* data, org_kde_plasma_window*, const char* name) {
        PLASMA_CTX(data);
        t->themedIcon = name ? name : "";
        if (t->ready) ctx->mgr->Changed();
    },
    .unmapped = [](void* data, org_kde_plasma_window*) {
        auto* ctx = static_cast<WlrHandleCtx*>(data);
        ToplevelManager* mgr = ctx->mgr;
        Toplevel* t = ctx->t;
        delete ctx;
        mgr->Remove(t);
    },
    .initial_state = [](void* data, org_kde_plasma_window*) {
        PLASMA_CTX(data);
        t->ready = true;
        ctx->mgr->Changed();
    },
    .parent_window = [](void* data, org_kde_plasma_window*, org_kde_plasma_window* parent) {
        PLASMA_CTX(data);
        t->hasParent = (parent != nullptr);
    },
    .geometry = [](void* data, org_kde_plasma_window*, int32_t x, int32_t y, uint32_t w, uint32_t h) {
        PLASMA_CTX(data);
        t->gx = x; t->gy = y; t->gw = static_cast<int>(w); t->gh = static_cast<int>(h);
        t->hasGeometry = true;   // no Changed(): this fires constantly while a window is dragged
    },
    .icon_changed = [](void*, org_kde_plasma_window*) {},
    .pid_changed = [](void* data, org_kde_plasma_window*, uint32_t pid) {
        PLASMA_CTX(data);
        t->pid = pid;
    },
    .virtual_desktop_entered = [](void* data, org_kde_plasma_window*, const char* id) {
        PLASMA_CTX(data);
        if (id) t->desktops.insert(id);
        if (t->ready) ctx->mgr->Changed();
    },
    .virtual_desktop_left = [](void* data, org_kde_plasma_window*, const char* id) {
        PLASMA_CTX(data);
        if (id) t->desktops.erase(id);
        if (t->ready) ctx->mgr->Changed();
    },
    .application_menu = [](void*, org_kde_plasma_window*, const char*, const char*) {},
    .activity_entered = [](void*, org_kde_plasma_window*, const char*) {},
    .activity_left = [](void*, org_kde_plasma_window*, const char*) {},
    .resource_name_changed = [](void*, org_kde_plasma_window*, const char*) {},
    .client_geometry = [](void*, org_kde_plasma_window*, int32_t, int32_t, uint32_t, uint32_t) {},
    .mapped = [](void*, org_kde_plasma_window*) {},
};

extern ToplevelManager gToplevels;

#endif
