#include "JpegWriter.h"

#include <jpeglib.h>

#include <csetjmp>
#include <cstdio>
#include <vector>

namespace hqcore {

namespace {
struct ErrorManager {
	jpeg_error_mgr pub;
	std::jmp_buf jumpBuffer;
};

void onJpegError(j_common_ptr cinfo) {
	// Default handler calls exit(); unwind back to writeJpegFromYuv420
	// instead so a failed write can't take the whole app down.
	auto *err = reinterpret_cast<ErrorManager *>(cinfo->err);
	std::longjmp(err->jumpBuffer, 1);
}
} // namespace

bool writeJpegFromYuv420(const std::string &path, int width, int height, const uint8_t *y,
			 int yStride, const uint8_t *u, const uint8_t *v, int uvStride,
			 int quality) {
	constexpr int kLumaRowsPerCall = 16; // == max_v_samp_factor * DCTSIZE for 4:2:0
	if (width < 2 || (width & 1) || height < kLumaRowsPerCall || (height % kLumaRowsPerCall) != 0)
		return false;

	std::FILE *file = std::fopen(path.c_str(), "wb");
	if (!file)
		return false;

	jpeg_compress_struct cinfo;
	ErrorManager errMgr;
	cinfo.err = jpeg_std_error(&errMgr.pub);
	errMgr.pub.error_exit = onJpegError;

	if (setjmp(errMgr.jumpBuffer)) {
		jpeg_destroy_compress(&cinfo);
		std::fclose(file);
		std::remove(path.c_str());
		return false;
	}

	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, file);

	cinfo.image_width = static_cast<JDIMENSION>(width);
	cinfo.image_height = static_cast<JDIMENSION>(height);
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	cinfo.raw_data_in = TRUE;
	// Input is already YCbCr - stop libjpeg trying to convert it, and set
	// the standard 4:2:0 sampling (Y 2x2, Cb/Cr 1x1).
	cinfo.jpeg_color_space = JCS_YCbCr;
	cinfo.comp_info[0].h_samp_factor = 2;
	cinfo.comp_info[0].v_samp_factor = 2;
	cinfo.comp_info[1].h_samp_factor = 1;
	cinfo.comp_info[1].v_samp_factor = 1;
	cinfo.comp_info[2].h_samp_factor = 1;
	cinfo.comp_info[2].v_samp_factor = 1;
	cinfo.dct_method = JDCT_ISLOW;

	jpeg_start_compress(&cinfo, TRUE);

	JSAMPROW yRows[16];
	JSAMPROW uRows[8];
	JSAMPROW vRows[8];
	JSAMPARRAY planes[3] = {yRows, uRows, vRows};

	while (cinfo.next_scanline < cinfo.image_height) {
		const int row = static_cast<int>(cinfo.next_scanline);
		for (int i = 0; i < 16; ++i)
			yRows[i] = const_cast<uint8_t *>(y) + static_cast<size_t>(row + i) * yStride;
		for (int i = 0; i < 8; ++i) {
			uRows[i] = const_cast<uint8_t *>(u) + static_cast<size_t>(row / 2 + i) * uvStride;
			vRows[i] = const_cast<uint8_t *>(v) + static_cast<size_t>(row / 2 + i) * uvStride;
		}
		jpeg_write_raw_data(&cinfo, planes, kLumaRowsPerCall);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	const bool closedOk = (std::fclose(file) == 0);
	if (!closedOk)
		std::remove(path.c_str());
	return closedOk;
}

} // namespace hqcore
