#pragma once

#include "Histogram.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hqcore {

// Output file format for a still capture.
enum class CaptureFormat { Dng, Jpg };

// The user-facing camera controls. All take effect on the live stream
// within a couple of frames.
struct CameraControls {
	int exposureUs = 20000;
	// White balance: multipliers applied to the red and blue channels (the
	// green channel is the reference, gain 1.0) - libcamera's "ColourGains".
	float redGain = 1.0f;
	float blueGain = 1.0f;
	float brightness = 0.0f; // -1.0 .. 1.0, 0 = unchanged
	float contrast = 1.0f;   // 0 .. 32, 1 = unchanged
	float saturation = 1.0f; // 0 .. 32, 1 = unchanged
};

// Ranges the camera accepts, as reported by libcamera for this sensor.
struct CameraControlRanges {
	int exposureMinUs = 100;
	int exposureMaxUs = 1000000;
	float gainMin = 0.0f;
	float gainMax = 32.0f;
	float brightnessMin = -1.0f;
	float brightnessMax = 1.0f;
	float contrastMin = 0.0f;
	float contrastMax = 32.0f;
	float saturationMin = 0.0f;
	float saturationMax = 32.0f;
};

// One live-preview frame, ISP-processed (white balance, brightness,
// contrast, saturation all applied) - packed RGB888, tightly packed rows.
struct PreviewFrame {
	int width = 0;
	int height = 0;
	// True for the click-to-inspect view: an unprocessed 1:1 crop of the
	// sensor's raw pixels (each 2x2 Bayer quad collapsed to one RGB pixel,
	// so the image is at half the sensor's linear resolution - display it
	// at 2x to see real sensor pixels at their true on-screen size).
	bool zoomed = false;
	std::vector<uint8_t> rgb;
};

// A full-resolution frame grabbed for saving. Which pixel fields are
// filled depends on the CaptureFormat the stream was started with.
struct StillCapture {
	CaptureFormat format = CaptureFormat::Dng;
	int width = 0;
	int height = 0;

	// --- CaptureFormat::Dng: the sensor's raw 12-bit Bayer data ---
	std::vector<uint16_t> raw; // width*height, values 0..4095, no padding
	std::array<uint8_t, 4> cfaPattern{0, 1, 1, 2}; // 0=R 1=G 2=B, reading order
	uint16_t blackLevel = 256;
	uint16_t whiteLevel = 4095;
	bool hasCcm = false;
	std::array<float, 9> ccm{1, 0, 0, 0, 1, 0, 0, 0, 1};

	// --- CaptureFormat::Jpg: ISP-processed planar YUV 4:2:0 ---
	std::vector<uint8_t> y, u, v;
	int yStride = 0;
	int uvStride = 0;

	// What the frame was actually exposed with (as reported by the camera
	// for this exact frame, not merely what was requested).
	int exposureUs = 0;
	float analogueGain = 1.0f;
	float redGain = 1.0f;
	float blueGain = 1.0f;
};

// Drives a libcamera-supported camera (built and tested with the Raspberry
// Pi HQ camera, IMX477, on a Pi 4B) with fully manual exposure and white
// balance - no auto-exposure / auto-white-balance, ever.
//
// Streams while running:
//  - a 12-bit raw stream at the sensor's full resolution (for the raw
//    histogram, DNG capture and the zoomed focus view),
//  - a small ISP-processed "lores" stream for the live preview,
//  - in CaptureFormat::Jpg only, a full-resolution ISP-processed stream
//    (JPEG source). DNG mode skips it, saving memory and ISP bandwidth.
//
// All callbacks run on libcamera's own thread; callers must marshal to
// their own threads (e.g. queued Qt signals).
class HqCamera {
public:
	// A preview frame, plus (if the histogram is enabled) the histogram of
	// the same frame's raw 12-bit values.
	using PreviewCallback = std::function<void(PreviewFrame, const Histogram *)>;

	// Opens the first camera libcamera finds. On failure returns nullptr and
	// fills `error` with a message fit to show the user.
	static std::unique_ptr<HqCamera> open(std::string &error);

	~HqCamera();
	HqCamera(const HqCamera &) = delete;
	HqCamera &operator=(const HqCamera &) = delete;

	const CameraControlRanges &ranges() const;
	// Sensor resolution of the raw stream (0 until start() succeeds).
	int rawWidth() const;
	int rawHeight() const;
	// Size of the (non-zoomed) preview frames.
	static constexpr int kPreviewWidth = 1014;
	static constexpr int kPreviewHeight = 760;

	// (Re)configures the streams for `format` and starts streaming. Safe to
	// call again to switch formats (stops first). Returns false and fills
	// `error` on failure.
	bool start(CaptureFormat format, PreviewCallback onPreview, std::string &error);
	void stop();
	bool isStreaming() const;
	CaptureFormat format() const;

	// Takes effect on the running stream within a few frames, and is
	// re-applied automatically across start()/stop() cycles.
	void setControls(const CameraControls &controls);

	// Histogram of the raw frame is only computed while enabled (it costs
	// a pass over the frame's memory); off by default.
	void setHistogramEnabled(bool enabled);

	// Zoom mode: while set, the preview shows the unprocessed 1:1 raw crop
	// (see PreviewFrame::zoomed) centred on this SENSOR-space (x, y).
	// std::nullopt returns to the normal preview.
	void setZoom(std::optional<std::pair<int, int>> sensorCenter);

	// Blocks until a frame whose exposure began AFTER this call - and after
	// the last setControls() had time to take effect - arrives, and returns
	// it (a copy; the camera keeps streaming). Guarantees the frame shows
	// the scene as it is now (e.g. film that has just stopped moving, LEDs
	// that have just switched on) rather than a stale buffered frame.
	// Returns std::nullopt on timeout or if the camera isn't streaming.
	std::optional<StillCapture> captureStill(int timeoutMs = 5000);

private:
	HqCamera();

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace hqcore
