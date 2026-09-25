#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hqcore {

// Number of distinct values a 12-bit raw sample can take.
constexpr size_t kRaw12Levels = 4096;

// A 4096-bin histogram over the sensor's true 12-bit raw values (0-4095).
// Deliberately colour-agnostic: every Bayer photosite counts equally,
// whatever its filter colour.
struct Histogram {
	std::array<uint32_t, kRaw12Levels> bins{};
};

// Adds the 12-bit samples of rows [0, height) of a raw frame into `out`
// but only every `rowPeriod`-th pair of rows (rows y where
// y % rowPeriod < 2) - a cheap, evenly-spread sub-sample that still
// covers both rows of the Bayer pattern (so all filter colours). Pass
// rowPeriod=2 to count every row.
//
// `rows` points at the first byte of row 0; `strideBytes` is the distance
// in bytes between rows (libcamera's stride, which can exceed
// width*2). Samples are unpacked 16-bit little-endian words; each is
// shifted right by `shift` (0 for LSB-aligned 12-bit data, 4 for
// MSB-aligned 16-bit) and anything above bit 11 is then masked off.
void accumulateRaw12Histogram(Histogram &out, const uint8_t *rows, size_t strideBytes,
			      int width, int height, int rowPeriod, int shift = 0);

} // namespace hqcore
