/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_UTIL_H
#define HDESKBAR_UTIL_H

#include <glib.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

extern bool gDebugEnabled;

inline void DebugLog(const char* fmt, ...)
{
	if (!gDebugEnabled)
		return;
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[hdeskbar] ");
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

inline void WarnLog(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[hdeskbar] ");
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

inline bool FileExists(const std::string& path)
{
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0;
}

inline bool IsDirectory(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline std::string ReadFile(const std::string& path)
{
	char* contents = nullptr;
	gsize length = 0;
	if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr))
		return std::string();
	std::string result(contents, length);
	g_free(contents);
	return result;
}

inline std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return std::string();
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

inline std::string HomeDir()
{
	return g_get_home_dir();
}

inline uint64_t NowMs()
{
	return g_get_monotonic_time() / 1000;
}

inline std::string ToLower(std::string s)
{
	for (auto& c : s)
		c = static_cast<char>(g_ascii_tolower(c));
	return s;
}

// Whether the desktop in use is called name (XDG_CURRENT_DESKTOP).
inline bool DesktopIs(const char* name)
{
	const char* current = g_getenv("XDG_CURRENT_DESKTOP");
	if (current == nullptr)
		return false;
	return ToLower(current).find(ToLower(name)) != std::string::npos;
}

#endif
