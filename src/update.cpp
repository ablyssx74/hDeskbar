/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "update.h"

#include <curl/curl.h>
#include <gio/gio.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "alert.h"
#include "settings.h"
#include "util.h"

namespace {

const char* const kVersionUrl = "https://raw.githubusercontent.com/ablyssx74/hDeskbar/refs/heads/main/VERSION";
const char* const kRepositoryUrl = "https://github.com/ablyssx74/hDeskbar";

size_t WriteCallback(void* contents, size_t size, size_t count, void* user)
{
	static_cast<std::string*>(user)->append(static_cast<char*>(contents), size * count);
	return size * count;
}

// v1.2.3 (or 1.2.3) as one number to compare.
int FlattenVersion(const char* text)
{
	int a = 0, b = 0, c = 0;
	const char* d = text;
	while (*d != '\0' && !g_ascii_isdigit(*d))
		d++;
	sscanf(d, "%d.%d.%d", &a, &b, &c);
	return a * 10000 + b * 100 + c;
}

// A desktop notification. done is told whether a notification server took it (there may be none, and then a window says it).
void SendNotification(const std::string& title, const std::string& body, std::function<void(bool)> done)
{
	GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
	if (bus == nullptr) {
		done(false);
		return;
	}
	GVariantBuilder actions, hints;
	g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
	g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hdeskbar"));
	auto* callback = new std::function<void(bool)>(std::move(done));
	g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
		"org.freedesktop.Notifications", "Notify",
		g_variant_new("(susssasa{sv}i)", "hDeskbar", 0u, "system-software-update", title.c_str(), body.c_str(), &actions, &hints, -1),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
		[](GObject* source, GAsyncResult* result, gpointer data) {
			auto* callback = static_cast<std::function<void(bool)>*>(data);
			GError* error = nullptr;
			GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
			if (reply != nullptr)
				g_variant_unref(reply);
			else
				DebugLog("notification not shown: %s\n", error != nullptr ? error->message : "unknown error");
			g_clear_error(&error);
			(*callback)(reply != nullptr);
			delete callback;
		}, callback);
	g_object_unref(bus);
}

}	// namespace

void StartUpdateChecker()
{
	if (!gSettings.checkForUpdates)
		return;
	std::thread([]() {
		// (the bar draws its first pictures first)
		std::this_thread::sleep_for(std::chrono::seconds(5));
		CURL* curl = curl_easy_init();
		if (curl == nullptr)
			return;
		const char* url = getenv("HDESKBAR_UPDATE_URL");		// (for testing)
		std::string buffer;
		curl_easy_setopt(curl, CURLOPT_URL, url != nullptr ? url : kVersionUrl);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "hdeskbar/1.0");
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		const CURLcode result = curl_easy_perform(curl);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);
		// (a file: URL has no HTTP status)
		if (result != CURLE_OK || (status != 200 && status != 0)) {
			DebugLog("update check failed: %s (HTTP %ld)\n", curl_easy_strerror(result), status);
			return;
		}
		char* trimmed = g_strstrip(g_strdup(buffer.c_str()));
		const std::string remote = trimmed;
		g_free(trimmed);
		DebugLog("update check: local %s, remote %s\n", APP_VERSION, remote.c_str());
		if (remote.empty() || FlattenVersion(remote.c_str()) <= FlattenVersion(APP_VERSION)) {
			DebugLog("update check: up to date\n");
			return;
		}
		auto* text = new std::string("A newer version of hDeskbar is available! (" + remote + ")");
		g_idle_add([](gpointer data) -> gboolean {
			auto* text = static_cast<std::string*>(data);
			const std::string message = *text;
			delete text;
			SendNotification("Update Available", message, [message](bool shown) {
				DebugLog("update notification %s\n", shown ? "shown" : "failed, showing an alert");
				if (shown)
					return;
				ShowAlert("Update Available", message + "\n\nYou are running " APP_VERSION ".", { "Later", "Open GitHub" },
					[](int choice) {
						if (choice == 1)
							g_spawn_command_line_async((std::string("xdg-open ") + kRepositoryUrl).c_str(), nullptr);
					}, 1);
			});
			return G_SOURCE_REMOVE;
		}, text);
	}).detach();
}
