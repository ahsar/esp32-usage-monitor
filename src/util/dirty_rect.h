#pragma once
// Bounding box of the pixels that differ between two RGB565 framebuffers.
// Header-only and dependency-free so the dirty-region push logic can be
// unit-tested on the host (see test/test_util/test_main.cpp).
//
// Pixels are compared two-at-a-time as 32-bit words, so the stride `w` must be
// even (every canvas/panel width in this project is). An odd width falls back
// to a full rect so a caller can never accidentally skip an update.
#include <stdint.h>
#include <stddef.h>

struct DirtyRect {
  int x0, y0;   // inclusive top-left  (meaningful only when !empty())
  int x1, y1;   // inclusive bottom-right

  bool empty() const { return x1 < x0 || y1 < y0; }
  int  w() const { return empty() ? 0 : x1 - x0 + 1; }
  int  h() const { return empty() ? 0 : y1 - y0 + 1; }
};

// Tight bounding box of every differing pixel, or an empty rect when the two
// buffers are identical. `w` must be even.
inline DirtyRect dirtyRect16(const uint16_t* cur, const uint16_t* prev,
                             int w, int h) {
  DirtyRect r{ w, h, -1, -1 };  // empty by construction
  if (w <= 0 || h <= 0) return r;
  if (w & 1) return DirtyRect{ 0, 0, w - 1, h - 1 };  // unsupported stride

  const int words = w >> 1;
  for (int y = 0; y < h; y++) {
    const uint16_t* c = cur + static_cast<size_t>(y) * w;
    const uint16_t* p = prev + static_cast<size_t>(y) * w;
    const uint32_t* a = reinterpret_cast<const uint32_t*>(c);
    const uint32_t* b = reinterpret_cast<const uint32_t*>(p);
    int rmin = -1, rmax = -1;
    for (int i = 0; i < words; i++) {
      if (a[i] != b[i]) {
        if (rmin < 0) rmin = i;
        rmax = i;
      }
    }
    if (rmin < 0) continue;
    // When only one pixel of a boundary word differs, trim the equal half so
    // the box hugs the change instead of the (coarser) word grid.
    int lo = rmin * 2;
    if (c[lo] == p[lo]) lo++;
    int hi = rmax * 2 + 1;
    if (c[hi] == p[hi]) hi--;
    if (y < r.y0) r.y0 = y;
    r.y1 = y;
    if (lo < r.x0) r.x0 = lo;
    if (hi > r.x1) r.x1 = hi;
  }
  return r;
}
