/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "shm.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "scale.h"
#include "wl.h"

namespace {

void BufferRelease(void*, wl_buffer* buffer)
{
	wl_buffer_destroy(buffer);
}

const wl_buffer_listener kBufferListener = { BufferRelease };

}	// namespace

wl_buffer* NewShmBuffer(int logicalWidth, int logicalHeight, const std::function<void(cairo_t*)>& draw)
{
	if (logicalWidth <= 0 || logicalHeight <= 0)
		return nullptr;
	const double scale = RenderScale();
	const int width = std::max(1, static_cast<int>(std::lround(logicalWidth * scale)));
	const int height = std::max(1, static_cast<int>(std::lround(logicalHeight * scale)));
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
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, width / static_cast<double>(logicalWidth), height / static_cast<double>(logicalHeight));
	draw(cr);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);

	wl_shm_pool* pool = wl_shm_create_pool(gWl.shm, fd, size);
	wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	wl_buffer_add_listener(buffer, &kBufferListener, nullptr);
	munmap(data, size);
	close(fd);
	return buffer;
}
