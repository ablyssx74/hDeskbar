/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The volume of the default output, through the PulseAudio API (served by pipewire-pulse on CachyOS). Changes are told by a
 * PulseAudio subscription, so nothing polls.
 */
#ifndef HDESKBAR_VOLUME_H
#define HDESKBAR_VOLUME_H

#include <glib.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "util.h"

class VolumeControl {
public:
    std::function<void()> onChanged;
    float level = 0.5f;   // 0..1 of PA_VOLUME_NORM (overdrive clamps to 1 for display)
    bool muted = false;
    bool ready = false;

    void Init() {
        fMainloop = pa_glib_mainloop_new(nullptr);
        Connect();
    }

    void SetLevel(float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        level = v;
        if (!ready || fSink.empty() || fChannels == 0) return;
        pa_cvolume cv;
        pa_cvolume_set(&cv, fChannels, static_cast<pa_volume_t>(std::lround(v * PA_VOLUME_NORM)));
        pa_operation* op = pa_context_set_sink_volume_by_name(fContext, fSink.c_str(), &cv, nullptr, nullptr);
        if (op) pa_operation_unref(op);
        if (muted && v > 0.0f) SetMuted(false);
        if (onChanged) onChanged();
    }

    void SetMuted(bool m) {
        muted = m;
        if (!ready || fSink.empty()) return;
        pa_operation* op = pa_context_set_sink_mute_by_name(fContext, fSink.c_str(), m ? 1 : 0, nullptr, nullptr);
        if (op) pa_operation_unref(op);
        if (onChanged) onChanged();
    }

private:
    void Connect() {
        if (fContext) {
            pa_context_disconnect(fContext);
            pa_context_unref(fContext);
        }
        ready = false;
        fContext = pa_context_new(pa_glib_mainloop_get_api(fMainloop), "hDeskbar");
        pa_context_set_state_callback(fContext, &StateCallback, this);
        pa_context_set_subscribe_callback(fContext, &SubscribeCallback, this);
        if (pa_context_connect(fContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) ScheduleReconnect();
    }

    void ScheduleReconnect() {
        if (fReconnectTimer) return;
        fReconnectTimer = g_timeout_add_seconds(3, [](gpointer p) -> gboolean {
            auto* self = static_cast<VolumeControl*>(p);
            self->fReconnectTimer = 0;
            self->Connect();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void RefreshServer() {
        pa_operation* op = pa_context_get_server_info(fContext, [](pa_context* c, const pa_server_info* info, void* p) {
            auto* self = static_cast<VolumeControl*>(p);
            if (info == nullptr || info->default_sink_name == nullptr) return;
            self->fSink = info->default_sink_name;
            self->RefreshSink();
        }, this);
        if (op) pa_operation_unref(op);
    }

    void RefreshSink() {
        if (fSink.empty()) return;
        pa_operation* op = pa_context_get_sink_info_by_name(fContext, fSink.c_str(),
            [](pa_context*, const pa_sink_info* info, int eol, void* p) {
                auto* self = static_cast<VolumeControl*>(p);
                if (eol || info == nullptr) return;
                self->fSinkIndex = info->index;
                self->fChannels = info->volume.channels;
                self->level = std::clamp(static_cast<float>(pa_cvolume_avg(&info->volume)) / PA_VOLUME_NORM, 0.0f, 1.0f);
                self->muted = info->mute;
                if (self->onChanged) self->onChanged();
            }, this);
        if (op) pa_operation_unref(op);
    }

    static void StateCallback(pa_context* c, void* p) {
        auto* self = static_cast<VolumeControl*>(p);
        switch (pa_context_get_state(c)) {
            case PA_CONTEXT_READY: {
                self->ready = true;
                pa_operation* op = pa_context_subscribe(c,
                    static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
                    nullptr, nullptr);
                if (op) pa_operation_unref(op);
                self->RefreshServer();
                DebugLog("PulseAudio connected\n");
                break;
            }
            case PA_CONTEXT_FAILED:
            case PA_CONTEXT_TERMINATED:
                self->ready = false;
                self->ScheduleReconnect();
                break;
            default:
                break;
        }
    }

    static void SubscribeCallback(pa_context*, pa_subscription_event_type_t type, uint32_t index, void* p) {
        auto* self = static_cast<VolumeControl*>(p);
        unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SERVER) self->RefreshServer();
        else if (facility == PA_SUBSCRIPTION_EVENT_SINK && index == self->fSinkIndex) self->RefreshSink();
    }

    pa_glib_mainloop* fMainloop = nullptr;
    pa_context* fContext = nullptr;
    std::string fSink;
    uint32_t fSinkIndex = PA_INVALID_INDEX;
    uint8_t fChannels = 0;
    guint fReconnectTimer = 0;
};


extern VolumeControl gVolume;

#endif
