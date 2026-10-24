/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The pill look of the menus, as hDesktop's Tracker draws it: the lit row and the snake trail, worked out per pixel and shaded
 * like a raised round bar.
 */
#ifndef HDESKBAR_PILL_H
#define HDESKBAR_PILL_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace PillGfx {


inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Coverage (0..1, anti-aliased) of a rounded rectangle at a pixel centre; each corner has its own radius.
inline float RoundRectCoverage(float px, float py, float x, float y, float w, float h, float tl, float tr, float br,
    float bl) {
    float hx = w / 2, hy = h / 2;
    float lx = px - (x + hx), ly = py - (y + hy);
    float r = (lx > 0) ? ((ly > 0) ? br : tr) : ((ly > 0) ? bl : tl);
    float qx = std::fabs(lx) - hx + r, qy = std::fabs(ly) - hy + r;
    float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
    float d = std::min(std::max(qx, qy), 0.0f) + outside - r;
    return Clamp01(0.5f - d);
}

// The concave "fillet" where the elbow bar meets a row: a square at the corner point (cx, cy), extending toward (dx, dy),
// minus the circle that rounds it.
inline float FilletCoverage(float px, float py, float cx, float cy, int dx, int dy, float r) {
    float centreX = cx + dx * r, centreY = cy + dy * r;
    float lx = px - (cx + dx * r / 2), ly = py - (cy + dy * r / 2);
    float qx = std::fabs(lx) - r / 2, qy = std::fabs(ly) - r / 2;
    float outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
    float dSquare = std::min(std::max(qx, qy), 0.0f) + outside;
    float dCircle = r - std::sqrt((px - centreX) * (px - centreX) + (py - centreY) * (py - centreY));
    return Clamp01(0.5f - std::max(dSquare, dCircle));
}

// Raises the lit shapes like a round bar (a quarter circle across the width, lit from the top left). `lit` is the coverage of
// the lit shapes (0..1); the result at each pixel is how much lighter (+) or darker (-) than flat it is. `scale` is the number
// of device pixels per logical one.
inline void RaisedShade(const std::vector<float>& lit, int w, int h, float scale, std::vector<float>& out) {
    const size_t n = static_cast<size_t>(w) * h;
    out.assign(n, 0.0f);
    std::vector<float> d(n, 0.0f);
    const float big = 1e4f;
    for (size_t k = 0; k < n; ++k) d[k] = lit[k] > 0.5f ? big : 0.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float& v = d[static_cast<size_t>(y) * w + x];
            if (v == 0.0f) continue;
            if (x > 0) v = std::min(v, d[static_cast<size_t>(y) * w + x - 1] + 1.0f);
            if (y > 0) v = std::min(v, d[static_cast<size_t>(y - 1) * w + x] + 1.0f);
            if (x > 0 && y > 0) v = std::min(v, d[static_cast<size_t>(y - 1) * w + x - 1] + 1.4142f);
            if (x + 1 < w && y > 0) v = std::min(v, d[static_cast<size_t>(y - 1) * w + x + 1] + 1.4142f);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            float& v = d[static_cast<size_t>(y) * w + x];
            if (v == 0.0f) continue;
            if (x + 1 < w) v = std::min(v, d[static_cast<size_t>(y) * w + x + 1] + 1.0f);
            if (y + 1 < h) v = std::min(v, d[static_cast<size_t>(y + 1) * w + x] + 1.0f);
            if (x + 1 < w && y + 1 < h) v = std::min(v, d[static_cast<size_t>(y + 1) * w + x + 1] + 1.4142f);
            if (x > 0 && y + 1 < h) v = std::min(v, d[static_cast<size_t>(y + 1) * w + x - 1] + 1.4142f);
        }
    const float R = 7.0f * scale;
    std::vector<float> hgt(n, 0.0f);
    for (size_t k = 0; k < n; ++k) {
        if (d[k] <= 0.0f) continue;
        float t = std::max(0.0f, std::min(1.0f, (d[k] - 0.5f) / R));
        hgt[k] = R * std::sqrt(std::max(0.0f, 1.0f - (1.0f - t) * (1.0f - t)));
    }
    std::vector<float> sm(n, 0.0f);   // a light blur takes the angles out of the distance map
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            int cnt = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    sum += hgt[static_cast<size_t>(yy) * w + xx];
                    ++cnt;
                }
            sm[static_cast<size_t>(y) * w + x] = sum / cnt;
        }
    // light from the top left
    const float lx = -0.45f, ly = -0.75f, lz = 0.55f;
    const float ln = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float strength = 0.55f;
    auto height = [&](int xx, int yy) {
        xx = std::max(0, std::min(w - 1, xx));
        yy = std::max(0, std::min(h - 1, yy));
        return sm[static_cast<size_t>(yy) * w + xx];
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t k = static_cast<size_t>(y) * w + x;
            if (lit[k] <= 0.0f) continue;
            float gx = (height(x + 1, y) - height(x - 1, y)) * 0.5f;
            float gy = (height(x, y + 1) - height(x, y - 1)) * 0.5f;
            float nn = std::sqrt(gx * gx + gy * gy + 1.0f);
            float dot = (-gx * lx - gy * ly + lz) / (nn * ln);
            out[k] = (dot - lz / ln) * strength * lit[k];
        }
}

struct Piece { float x, y, w, h, tl, tr, br, bl; };
struct Fillet { float x, y; int dx, dy; };
}	// namespace PillGfx

#endif
