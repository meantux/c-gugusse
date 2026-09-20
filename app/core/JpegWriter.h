#pragma once

#include <cstdint>
#include <string>

namespace hqcore {

// Writes a baseline JPEG straight from planar YUV 4:2:0 data (what the
// Raspberry Pi ISP produces natively for a full-resolution stream), via
// libjpeg's raw-data interface - no YUV->RGB conversion pass, so a 12MP
// frame encodes in well under a second on a Pi 4.
//
// The YUV data is the ISP's "sYCC" (full-range JFIF-style YCbCr), which is
// exactly what JPEG stores, so it goes in as-is.
//
// Plane layout: `y` is `height` rows of `yStride` bytes (>= width);
// `u`/`v` are `height/2` rows of `uvStride` bytes (>= width/2). Strides
// must be large enough that reading up to the next multiple of 8
// samples past a row's end (libjpeg's block padding) stays in-buffer -
// libcamera's 32-byte-aligned strides always are.
//
// `width` must be even and `height` a multiple of 16. Returns false on
// any failure (bad dimensions, file I/O error, libjpeg error).
bool writeJpegFromYuv420(const std::string &path, int width, int height, const uint8_t *y,
			 int yStride, const uint8_t *u, const uint8_t *v, int uvStride,
			 int quality);

} // namespace hqcore
