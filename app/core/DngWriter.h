#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hqcore {

// Everything (besides the pixels) a DNG needs to describe a raw Bayer
// frame from a libcamera-driven sensor.
struct DngMetadata {
	// Colour of each photosite in the repeating 2x2 pattern, in reading
	// order (top-left, top-right, bottom-left, bottom-right): 0=R, 1=G, 2=B.
	std::array<uint8_t, 4> cfaPattern{0, 1, 1, 2};

	// Sample range: the sensor's black level (the value a fully dark pixel
	// reads) and the maximum value (4095 for a 12-bit sensor).
	uint16_t blackLevel = 256;
	uint16_t whiteLevel = 4095;

	// White balance applied by the ISP for this frame (multipliers on the
	// R and B channels, G is 1.0) - stored in the DNG as the "as shot"
	// neutral so raw developers start from the same white balance.
	float redGain = 1.0f;
	float blueGain = 1.0f;

	// libcamera's colour correction matrix for this frame: white-balanced
	// camera RGB -> linear sRGB, row-major. Optional - identity if the
	// IPA didn't report one.
	bool hasCcm = false;
	std::array<float, 9> ccm{1, 0, 0, 0, 1, 0, 0, 0, 1};

	// Orientation set by the operator in the preview (hflip/vflip toggle
	// buttons). The raw pixels are written unchanged - this only sets the
	// DNG's Orientation tag, which viewers/editors apply on load.
	bool hFlip = false;
	bool vFlip = false;

	std::string make = "Raspberry Pi";
	std::string model = "HQ Camera";
	std::string description; // ImageDescription tag; free text
};

// Writes an uncompressed, single-image DNG via libtiff, with the sensor's
// 12-bit values packed (two pixels in three bytes, 1.5 bytes/pixel).
// `pixels` holds width*height samples in the low 12 bits of each uint16,
// row-major, no padding; `width` must be even.
//
// Returns false on any failure (bad size, I/O error, libtiff error).
bool writeDng(const std::string &path, int width, int height,
	      const std::vector<uint16_t> &pixels, const DngMetadata &meta);

} // namespace hqcore
