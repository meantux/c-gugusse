#include "DngWriter.h"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hqcore {

namespace {

using Mat3 = std::array<double, 9>; // row-major

Mat3 multiply(const Mat3 &a, const Mat3 &b) {
	Mat3 out{};
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			for (int k = 0; k < 3; ++k)
				out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
	return out;
}

// Returns false if `m` is (numerically) singular.
bool invert(const Mat3 &m, Mat3 &out) {
	const double a = m[0], b = m[1], c = m[2];
	const double d = m[3], e = m[4], f = m[5];
	const double g = m[6], h = m[7], i = m[8];
	const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1e-12)
		return false;
	const double inv = 1.0 / det;
	out = {(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
	       (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
	       (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv};
	return true;
}

// Linear sRGB (D65) -> CIE XYZ (D65).
const Mat3 kSrgbToXyz = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
			 0.0721750, 0.0193339, 0.1191920, 0.9503041};

constexpr uint16_t kIlluminantD65 = 21; // EXIF LightSource / DNG CalibrationIlluminant

constexpr int kRowsPerStrip = 64;

// Packs 12-bit samples MSB-first, two pixels in three bytes (the TIFF/DNG
// layout for BitsPerSample=12):  p0[11:4] | p0[3:0]p1[11:8] | p1[7:0].
// `count` must be even. Bits above bit 11 are ignored.
void pack12(const uint16_t *in, size_t count, uint8_t *out) {
	for (size_t i = 0; i < count; i += 2) {
		const uint16_t a = in[i] & 0x0FFF;
		const uint16_t b = in[i + 1] & 0x0FFF;
		*out++ = static_cast<uint8_t>(a >> 4);
		*out++ = static_cast<uint8_t>(((a & 0x0F) << 4) | (b >> 8));
		*out++ = static_cast<uint8_t>(b & 0xFF);
	}
}

} // namespace

bool writeDng(const std::string &path, int width, int height,
	      const std::vector<uint16_t> &pixels, const DngMetadata &meta) {
	if (width < 2 || height < 2 || (width & 1) ||
	    pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
		return false;
	}

	// libcamera's colour correction matrix maps white-balanced camera RGB
	// to linear sRGB:  sRGB = CCM * diag(redGain, 1, blueGain) * camera.
	// DNG's ColorMatrix1 goes the other way (XYZ -> camera), so:
	//   XYZ = sRGB->XYZ * CCM * WB * camera  =>  camera = inv(...) * XYZ.
	// (The same derivation rpicam-apps uses for its DNG output.)
	const Mat3 ccm = meta.hasCcm ? Mat3{meta.ccm[0], meta.ccm[1], meta.ccm[2], meta.ccm[3],
					    meta.ccm[4], meta.ccm[5], meta.ccm[6], meta.ccm[7],
					    meta.ccm[8]}
				      : Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
	const double redGain = meta.redGain > 0.0f ? meta.redGain : 1.0f;
	const double blueGain = meta.blueGain > 0.0f ? meta.blueGain : 1.0f;
	const Mat3 whiteBalance = {redGain, 0, 0, 0, 1, 0, 0, 0, blueGain};

	Mat3 xyzToCamera{};
	if (!invert(multiply(kSrgbToXyz, multiply(ccm, whiteBalance)), xyzToCamera))
		return false;
	float colorMatrix[9];
	for (int i = 0; i < 9; ++i)
		colorMatrix[i] = static_cast<float>(xyzToCamera[i]);

	// White in camera space: what a neutral surface reads before white
	// balance was applied.
	const float asShotNeutral[3] = {static_cast<float>(1.0 / redGain), 1.0f,
					static_cast<float>(1.0 / blueGain)};

	TIFF *tif = TIFFOpen(path.c_str(), "w");
	if (!tif)
		return false;

	auto fail = [&] {
		TIFFClose(tif);
		std::remove(path.c_str());
		return false;
	};

	const uint16_t cfaRepeatDim[2] = {2, 2};
	const float blackLevel[4] = {static_cast<float>(meta.blackLevel),
				     static_cast<float>(meta.blackLevel),
				     static_cast<float>(meta.blackLevel),
				     static_cast<float>(meta.blackLevel)};
	const uint16_t blackRepeatDim[2] = {2, 2};
	const uint32_t whiteLevel = meta.whiteLevel;

	bool ok = true;
	ok &= TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width)) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height)) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 12) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, kRowsPerStrip) == 1;
	// TIFF/DNG Orientation values match EXIF's: mirroring only (no
	// rotation) is all the hflip/vflip toggles need - TOPLEFT(1)/
	// TOPRIGHT(2, mirror horizontal)/BOTRIGHT(3, mirror both = 180
	// degrees)/BOTLEFT(4, mirror vertical).
	const uint16_t orientation = meta.hFlip
					      ? (meta.vFlip ? ORIENTATION_BOTRIGHT : ORIENTATION_TOPRIGHT)
					      : (meta.vFlip ? ORIENTATION_BOTLEFT : ORIENTATION_TOPLEFT);
	ok &= TIFFSetField(tif, TIFFTAG_ORIENTATION, orientation) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_MAKE, meta.make.c_str()) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_MODEL, meta.model.c_str()) == 1;
	if (!meta.description.empty())
		ok &= TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, meta.description.c_str()) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_SOFTWARE, "gugusse hq capture") == 1;

	ok &= TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfaRepeatDim) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, meta.cfaPattern.data()) == 1;

	ok &= TIFFSetField(tif, TIFFTAG_DNGVERSION, "\001\004\000\000") == 1;
	ok &= TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, "\001\001\000\000") == 1;
	ok &= TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL,
			   (meta.make + " " + meta.model).c_str()) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, blackRepeatDim) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, blackLevel) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whiteLevel) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colorMatrix) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, asShotNeutral) == 1;
	ok &= TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, kIlluminantD65) == 1;
	if (!ok)
		return fail();

	// 12 bits per sample, packed: 3 bytes per pair of pixels.
	const size_t rowBytes = static_cast<size_t>(width) / 2 * 3;
	std::vector<uint8_t> packed(rowBytes * kRowsPerStrip);
	for (int row = 0; row < height; row += kRowsPerStrip) {
		const int rows = std::min(kRowsPerStrip, height - row);
		const tmsize_t bytes = static_cast<tmsize_t>(rowBytes) * rows;
		pack12(pixels.data() + static_cast<size_t>(row) * width,
		       static_cast<size_t>(rows) * width, packed.data());
		if (TIFFWriteEncodedStrip(tif, static_cast<tstrip_t>(row / kRowsPerStrip),
					  packed.data(), bytes) == -1) {
			return fail();
		}
	}

	if (!TIFFWriteDirectory(tif))
		return fail();
	TIFFClose(tif);
	return true;
}

} // namespace hqcore
