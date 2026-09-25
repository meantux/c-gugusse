#include "HqCamera.h"

#include <libcamera/libcamera.h>

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <mutex>

using namespace libcamera;

namespace hqcore {

namespace {

// Requests in flight. The raw stream is ~25MB a frame, so this is a
// memory/latency trade-off: enough that a slow callback (a still-frame
// copy) doesn't starve the sensor, few enough to stay well inside the
// Pi 4's 512MB CMA pool even with the extra full-res stream in JPG mode.
constexpr unsigned kBufferCount = 4;

// After setControls() changes something, frames exposed within this long
// are not trusted to reflect it (ISP-side controls like contrast and
// saturation land a few frames after the sensor-side ones).
constexpr uint64_t kControlSettleNs = 250'000'000;

// captureStill() normally waits for a frame that provably has the
// requested exposure/gains; after this many frames without one it takes
// the next frame anyway rather than hang forever (e.g. an exposure the
// sensor cannot reach).
constexpr int kMaxStillFramesToWait = 12;

// Histogram sub-sampling: every 16th pair of rows, all columns - see
// accumulateRaw12Histogram().
constexpr int kHistogramRowPeriod = 16;

// Zoom view: this many raw sensor pixels, shown 1:1 (see PreviewFrame).
constexpr int kZoomRawWidth = 960;
constexpr int kZoomRawHeight = 644;

uint64_t nowNs() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

// libcamera control ranges come back as ControlValue; only trust plain
// scalars.
template <typename T> std::optional<T> scalarOf(const ControlValue &value) {
	if (value.isArray() || value.type() != details::control_type<T>::value)
		return std::nullopt;
	return value.get<T>();
}

// BT.601 full-range YCbCr -> RGB (libcamera's "sYCC" colour space, the
// same convention JPEG uses), via small lookup tables.
struct YuvTables {
	int rv[256], gu[256], gv[256], bu[256];
	YuvTables() {
		for (int i = 0; i < 256; ++i) {
			const double d = i - 128;
			rv[i] = static_cast<int>(std::lround(1.402 * d * 65536));
			gu[i] = static_cast<int>(std::lround(-0.344136 * d * 65536));
			gv[i] = static_cast<int>(std::lround(-0.714136 * d * 65536));
			bu[i] = static_cast<int>(std::lround(1.772 * d * 65536));
		}
	}
};

uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// sRGB-ish display curve for the linear raw zoom view.
struct GammaLut {
	uint8_t v[4096];
	GammaLut() {
		for (int i = 0; i < 4096; ++i)
			v[i] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 4095.0, 1.0 / 2.2)));
	}
};

struct FrameMeta {
	std::optional<uint64_t> timestampNs;
	std::optional<int> exposureUs;
	std::optional<float> analogueGain;
	std::optional<float> redGain;
	std::optional<float> blueGain;
	std::optional<uint16_t> blackLevel12;
	std::optional<std::array<float, 9>> ccm;
};

struct MappedBuffer {
	std::vector<std::pair<uint8_t *, size_t>> planes;    // plane start, length
	std::vector<std::pair<void *, size_t>> mappings;     // for munmap
};

struct PendingStill {
	uint64_t notBeforeNs = 0;
	CameraControls expected;
	int framesSeen = 0;
	std::promise<std::optional<StillCapture>> promise;
};

} // namespace

struct HqCamera::Impl {
	~Impl() { unmapAll(); }

	std::unique_ptr<CameraManager> manager;
	std::shared_ptr<Camera> camera;
	CameraControlRanges ranges;
	bool supportsNoiseReduction = false;

	// --- Streaming state (guarded by lifecycleMutex for start/stop) ---
	std::mutex lifecycleMutex;
	std::atomic<bool> streaming{false};
	CaptureFormat format = CaptureFormat::Dng;
	std::unique_ptr<CameraConfiguration> config;
	Stream *rawStream = nullptr;
	Stream *previewStream = nullptr;
	Stream *mainStream = nullptr; // JPG mode only
	int rawWidth = 0;
	int rawHeight = 0;
	size_t rawStride = 0;
	// Right shift that brings a raw sample down to its 12-bit value: 0 when
	// the samples are LSB-aligned 12-bit (Pi 4), 4 when they arrive as
	// MSB-aligned 16-bit (Pi 5 - its CSI-2 receiver unpacks that way).
	int rawShift = 0;
	int previewWidth = 0;
	int previewHeight = 0;
	size_t previewStride = 0;
	size_t mainYStride = 0;
	std::array<uint8_t, 4> cfa{0, 1, 1, 2};
	std::unique_ptr<FrameBufferAllocator> allocator;
	std::vector<std::unique_ptr<Request>> requests;
	std::map<const FrameBuffer *, MappedBuffer> mapped;
	PreviewCallback onPreview;

	// --- Live controls ---
	mutable std::mutex controlsMutex;
	CameraControls controls;
	uint64_t controlsChangedNs = 0;
	std::atomic<bool> histogramEnabled{false};

	std::mutex zoomMutex;
	std::optional<std::pair<int, int>> zoom;

	std::mutex stillMutex;
	std::shared_ptr<PendingStill> pendingStill;

	// Scratch copies in ordinary (cached) heap memory. The camera's DMA
	// buffers are mapped uncached, where scattered per-pixel reads are
	// ~50x slower than a bulk memcpy - so every consumer below memcpy()s
	// the bytes it needs into these first and works on the copy. Only
	// touched from the camera callback thread.
	std::vector<uint8_t> scratchY, scratchU, scratchV, scratchRaw;

	void unmapAll() {
		for (auto &entry : mapped)
			for (auto &m : entry.second.mappings)
				munmap(m.first, m.second);
		mapped.clear();
	}

	bool mapBuffer(const FrameBuffer *buffer) {
		MappedBuffer mb;
		// A buffer's planes usually share one dmabuf fd at different
		// offsets - map each distinct fd once, over its furthest extent.
		std::map<int, size_t> extents;
		for (const auto &plane : buffer->planes()) {
			const int fd = plane.fd.get();
			extents[fd] = std::max<size_t>(extents[fd], plane.offset + plane.length);
		}
		std::map<int, uint8_t *> bases;
		for (const auto &[fd, length] : extents) {
			void *p = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
			if (p == MAP_FAILED) {
				for (auto &m : mb.mappings)
					munmap(m.first, m.second);
				return false;
			}
			mb.mappings.emplace_back(p, length);
			bases[fd] = static_cast<uint8_t *>(p);
		}
		for (const auto &plane : buffer->planes()) {
			mb.planes.emplace_back(bases[plane.fd.get()] + plane.offset, plane.length);
		}
		mapped[buffer] = std::move(mb);
		return true;
	}

	bool supports(const ControlId &id) const {
		return camera->controls().find(&id) != camera->controls().end();
	}

	void fillControls(ControlList &list) {
		CameraControls c;
		{
			std::lock_guard<std::mutex> lock(controlsMutex);
			c = controls;
		}
		list.set(controls::AeEnable, false);
		list.set(controls::AwbEnable, false);
		list.set(controls::ExposureTime, c.exposureUs);
		list.set(controls::AnalogueGain, 1.0f);
		const float gains[2] = {c.redGain, c.blueGain};
		list.set(controls::ColourGains, Span<const float, 2>(gains));
		list.set(controls::Brightness, c.brightness);
		list.set(controls::Contrast, c.contrast);
		list.set(controls::Saturation, c.saturation);
		if (supportsNoiseReduction) {
			// Off, like ../GugusseRoller: noise reduction would smear
			// film grain (and is not applied to raw anyway).
			list.set(controls::draft::NoiseReductionMode,
				 controls::draft::NoiseReductionModeOff);
		}
	}

	static FrameMeta readMeta(const ControlList &md) {
		FrameMeta m;
		if (auto v = md.get(controls::SensorTimestamp))
			m.timestampNs = static_cast<uint64_t>(*v);
		if (auto v = md.get(controls::ExposureTime))
			m.exposureUs = *v;
		if (auto v = md.get(controls::AnalogueGain))
			m.analogueGain = *v;
		if (auto v = md.get(controls::ColourGains)) {
			m.redGain = (*v)[0];
			m.blueGain = (*v)[1];
		}
		if (auto v = md.get(controls::SensorBlackLevels)) {
			// Reported on a 16-bit scale; our samples are 12-bit.
			m.blackLevel12 = static_cast<uint16_t>((*v)[0] >> 4);
		}
		if (auto v = md.get(controls::ColourCorrectionMatrix)) {
			std::array<float, 9> ccm{};
			for (size_t i = 0; i < 9 && i < v->size(); ++i)
				ccm[i] = (*v)[i];
			m.ccm = ccm;
		}
		return m;
	}

	bool stillAccepts(const PendingStill &p, const FrameMeta &m) const {
		if (p.framesSeen >= kMaxStillFramesToWait)
			return true;
		if (m.timestampNs) {
			// The timestamp marks the start of readout; the exposure ran
			// for exposureUs before that.
			const uint64_t exposureNs =
				static_cast<uint64_t>(m.exposureUs.value_or(0)) * 1000ull;
			const uint64_t exposureStart =
				*m.timestampNs > exposureNs ? *m.timestampNs - exposureNs : 0;
			if (exposureStart < p.notBeforeNs)
				return false;
		}
		if (m.exposureUs) {
			const double tol = std::max(60.0, p.expected.exposureUs * 0.03);
			if (std::fabs(*m.exposureUs - p.expected.exposureUs) > tol)
				return false;
		}
		auto gainOk = [](std::optional<float> got, float want) {
			return !got || std::fabs(*got - want) <= std::max(0.01f, want * 0.02f);
		};
		return gainOk(m.redGain, p.expected.redGain) && gainOk(m.blueGain, p.expected.blueGain);
	}

	std::optional<StillCapture> grabStill(Request *request, const FrameMeta &meta) {
		StillCapture s;
		s.format = format;
		s.width = rawWidth;
		s.height = rawHeight;
		s.exposureUs = meta.exposureUs.value_or(0);
		s.analogueGain = meta.analogueGain.value_or(1.0f);
		s.redGain = meta.redGain.value_or(1.0f);
		s.blueGain = meta.blueGain.value_or(1.0f);

		if (format == CaptureFormat::Dng) {
			const auto it = mapped.find(request->findBuffer(rawStream));
			if (it == mapped.end())
				return std::nullopt;
			const uint8_t *src = it->second.planes[0].first;
			s.raw.resize(static_cast<size_t>(rawWidth) * rawHeight);
			for (int row = 0; row < rawHeight; ++row) {
				std::memcpy(s.raw.data() + static_cast<size_t>(row) * rawWidth,
					    src + static_cast<size_t>(row) * rawStride,
					    static_cast<size_t>(rawWidth) * sizeof(uint16_t));
			}
			// Unpacked 12-bit samples should never exceed 4095; mask so a
			// stray high bit can't produce out-of-range DNG values.
			for (auto &v : s.raw)
				v = (v >> rawShift) & 0x0FFF;
			s.cfaPattern = cfa;
			s.blackLevel = meta.blackLevel12.value_or(256);
			s.whiteLevel = 4095;
			if (meta.ccm) {
				s.hasCcm = true;
				s.ccm = *meta.ccm;
			}
		} else {
			const auto it = mapped.find(request->findBuffer(mainStream));
			if (it == mapped.end() || it->second.planes.size() < 3)
				return std::nullopt;
			const auto &planes = it->second.planes;
			s.yStride = static_cast<int>(mainYStride);
			s.uvStride = s.yStride / 2;
			s.y.assign(planes[0].first, planes[0].first + planes[0].second);
			s.u.assign(planes[1].first, planes[1].first + planes[1].second);
			s.v.assign(planes[2].first, planes[2].first + planes[2].second);
		}
		return s;
	}

	PreviewFrame makePreview(Request *request) {
		static const YuvTables tables;
		PreviewFrame frame;
		const auto it = mapped.find(request->findBuffer(previewStream));
		if (it == mapped.end() || it->second.planes.size() < 3)
			return frame;
		const auto &planes = it->second.planes;
		const size_t uvStride = previewStride / 2;

		auto pull = [](std::vector<uint8_t> &dst, const uint8_t *src, size_t bytes) {
			dst.resize(bytes);
			std::memcpy(dst.data(), src, bytes);
		};
		pull(scratchY, planes[0].first, previewStride * previewHeight);
		pull(scratchU, planes[1].first, uvStride * (previewHeight / 2));
		pull(scratchV, planes[2].first, uvStride * (previewHeight / 2));
		const uint8_t *yPlane = scratchY.data();
		const uint8_t *uPlane = scratchU.data();
		const uint8_t *vPlane = scratchV.data();

		frame.width = previewWidth;
		frame.height = previewHeight;
		frame.rgb.resize(static_cast<size_t>(previewWidth) * previewHeight * 3);
		uint8_t *out = frame.rgb.data();
		for (int row = 0; row < previewHeight; ++row) {
			const uint8_t *yRow = yPlane + static_cast<size_t>(row) * previewStride;
			const uint8_t *uRow = uPlane + static_cast<size_t>(row / 2) * uvStride;
			const uint8_t *vRow = vPlane + static_cast<size_t>(row / 2) * uvStride;
			for (int col = 0; col < previewWidth; ++col) {
				const int Y = yRow[col] << 16;
				const int U = uRow[col / 2];
				const int V = vRow[col / 2];
				*out++ = clamp8((Y + tables.rv[V] + 32768) >> 16);
				*out++ = clamp8((Y + tables.gu[U] + tables.gv[V] + 32768) >> 16);
				*out++ = clamp8((Y + tables.bu[U] + 32768) >> 16);
			}
		}
		return frame;
	}

	PreviewFrame makeZoomPreview(Request *request, std::pair<int, int> center) {
		static const GammaLut gamma;
		PreviewFrame frame;
		const auto it = mapped.find(request->findBuffer(rawStream));
		if (it == mapped.end())
			return frame;
		const uint8_t *rawMapped = it->second.planes[0].first;

		CameraControls c;
		{
			std::lock_guard<std::mutex> lock(controlsMutex);
			c = controls;
		}

		const int cropW = std::min(kZoomRawWidth, rawWidth) & ~1;
		const int cropH = std::min(kZoomRawHeight, rawHeight) & ~1;
		const int x0 = std::clamp(center.first - cropW / 2, 0, rawWidth - cropW) & ~1;
		const int y0 = std::clamp(center.second - cropH / 2, 0, rawHeight - cropH) & ~1;

		// Pull just the crop's rows (each a contiguous run) into scratch;
		// `raw` then addresses the crop with the same row math as the
		// full frame, minus the x0/y0 offsets.
		const size_t cropRowBytes = static_cast<size_t>(cropW) * 2;
		scratchRaw.resize(cropRowBytes * cropH);
		for (int row = 0; row < cropH; ++row) {
			std::memcpy(scratchRaw.data() + row * cropRowBytes,
				    rawMapped + static_cast<size_t>(y0 + row) * rawStride +
					    static_cast<size_t>(x0) * 2,
				    cropRowBytes);
		}
		const uint8_t *raw = scratchRaw.data();

		frame.zoomed = true;
		frame.width = cropW / 2;
		frame.height = cropH / 2;
		frame.rgb.resize(static_cast<size_t>(frame.width) * frame.height * 3);

		const int black = 256;
		const float scale = 4095.0f / (4095 - black);
		auto toDisplay = [&](float v, float gain) {
			const int idx = static_cast<int>(std::clamp((v - black) * gain * scale, 0.0f, 4095.0f));
			return gamma.v[idx];
		};

		uint8_t *out = frame.rgb.data();
		for (int oy = 0; oy < frame.height; ++oy) {
			const uint8_t *row0 = raw + static_cast<size_t>(oy * 2) * cropRowBytes;
			const uint8_t *row1 = row0 + cropRowBytes;
			for (int ox = 0; ox < frame.width; ++ox) {
				const size_t xb = static_cast<size_t>(ox * 2) * 2;
				uint16_t s[4];
				std::memcpy(&s[0], row0 + xb, 2);
				std::memcpy(&s[1], row0 + xb + 2, 2);
				std::memcpy(&s[2], row1 + xb, 2);
				std::memcpy(&s[3], row1 + xb + 2, 2);
				float r = 0, g = 0, b = 0;
				int greens = 0;
				for (int k = 0; k < 4; ++k) {
					const float v = (s[k] >> rawShift) & 0x0FFF;
					if (cfa[k] == 0)
						r = v;
					else if (cfa[k] == 2)
						b = v;
					else {
						g += v;
						++greens;
					}
				}
				if (greens)
					g /= greens;
				*out++ = toDisplay(r, c.redGain);
				*out++ = toDisplay(g, 1.0f);
				*out++ = toDisplay(b, c.blueGain);
			}
		}
		return frame;
	}

	void onRequestCompleted(Request *request) {
		if (request->status() == Request::RequestCancelled || !streaming)
			return;

		const FrameMeta meta = readMeta(request->metadata());

		std::shared_ptr<PendingStill> still;
		{
			std::lock_guard<std::mutex> lock(stillMutex);
			if (pendingStill) {
				++pendingStill->framesSeen;
				if (stillAccepts(*pendingStill, meta)) {
					still = std::move(pendingStill);
					pendingStill.reset();
				}
			}
		}
		if (still)
			still->promise.set_value(grabStill(request, meta));

		if (onPreview) {
			std::optional<std::pair<int, int>> zoomCenter;
			{
				std::lock_guard<std::mutex> lock(zoomMutex);
				zoomCenter = zoom;
			}
			PreviewFrame frame =
				zoomCenter ? makeZoomPreview(request, *zoomCenter) : makePreview(request);

			Histogram hist;
			bool haveHist = false;
			if (histogramEnabled) {
				const auto it = mapped.find(request->findBuffer(rawStream));
				if (it != mapped.end()) {
					// Bulk-copy each sampled pair of rows (contiguous in
					// the buffer) out of the uncached mapping, then count
					// from the copy.
					const uint8_t *src = it->second.planes[0].first;
					scratchRaw.resize(rawStride * 2);
					for (int y = 0; y + 1 < rawHeight; y += kHistogramRowPeriod) {
						std::memcpy(scratchRaw.data(), src + static_cast<size_t>(y) * rawStride,
							    rawStride * 2);
						accumulateRaw12Histogram(hist, scratchRaw.data(), rawStride,
									 rawWidth, 2, 2, rawShift);
					}
					haveHist = true;
				}
			}
			if (frame.width > 0 && frame.height > 0)
				onPreview(std::move(frame), haveHist ? &hist : nullptr);
		}

		// stop() clears `streaming` before stopping the camera; don't
		// re-queue into a camera that is on its way down.
		if (!streaming)
			return;
		request->reuse(Request::ReuseBuffers);
		fillControls(request->controls());
		camera->queueRequest(request);
	}
};

HqCamera::HqCamera() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<HqCamera> HqCamera::open(std::string &error) {
	// libcamera is chatty at INFO level; keep warnings/errors only unless
	// the user asked otherwise.
	setenv("LIBCAMERA_LOG_LEVELS", "*:WARN", 0);

	std::unique_ptr<HqCamera> cam(new HqCamera());
	Impl &impl = *cam->impl_;

	impl.manager = std::make_unique<CameraManager>();
	if (impl.manager->start() != 0) {
		error = "libcamera failed to start.";
		return nullptr;
	}
	if (impl.manager->cameras().empty()) {
		error = "No camera found. Check the ribbon cable and that "
			"\"rpicam-hello --list-cameras\" lists the camera.";
		return nullptr;
	}
	impl.camera = impl.manager->cameras()[0];
	if (impl.camera->acquire() != 0) {
		error = "The camera is in use by another application (is another "
			"instance or rpicam-* tool running?).";
		return nullptr;
	}

	const ControlInfoMap &info = impl.camera->controls();
	CameraControlRanges &r = impl.ranges;
	if (auto it = info.find(&controls::ExposureTime); it != info.end()) {
		if (auto v = scalarOf<int32_t>(it->second.min()))
			r.exposureMinUs = *v;
		if (auto v = scalarOf<int32_t>(it->second.max()))
			r.exposureMaxUs = *v;
	}
	auto floatRange = [&](const ControlId &id, float &lo, float &hi) {
		auto it = info.find(&id);
		if (it == info.end())
			return;
		if (auto v = scalarOf<float>(it->second.min()))
			lo = *v;
		if (auto v = scalarOf<float>(it->second.max()))
			hi = *v;
	};
	floatRange(controls::ColourGains, r.gainMin, r.gainMax);
	floatRange(controls::Brightness, r.brightnessMin, r.brightnessMax);
	floatRange(controls::Contrast, r.contrastMin, r.contrastMax);
	floatRange(controls::Saturation, r.saturationMin, r.saturationMax);
	impl.supportsNoiseReduction = impl.supports(controls::draft::NoiseReductionMode);

	impl.controls.exposureUs = std::clamp(impl.controls.exposureUs, r.exposureMinUs, r.exposureMaxUs);
	return cam;
}

HqCamera::~HqCamera() {
	if (!impl_)
		return;
	stop();
	if (impl_->camera)
		impl_->camera->release();
	impl_->camera.reset();
	if (impl_->manager)
		impl_->manager->stop();
}

const CameraControlRanges &HqCamera::ranges() const { return impl_->ranges; }
int HqCamera::rawWidth() const { return impl_->rawWidth; }
int HqCamera::rawHeight() const { return impl_->rawHeight; }
bool HqCamera::isStreaming() const { return impl_->streaming; }
CaptureFormat HqCamera::format() const { return impl_->format; }

bool HqCamera::start(CaptureFormat format, PreviewCallback onPreview, std::string &error) {
	stop();
	Impl &s = *impl_;
	std::lock_guard<std::mutex> lifecycle(s.lifecycleMutex);

	std::vector<StreamRole> roles = {StreamRole::Raw};
	if (format == CaptureFormat::Jpg)
		roles.push_back(StreamRole::StillCapture);
	roles.push_back(StreamRole::Viewfinder);

	s.config = s.camera->generateConfiguration(roles);
	if (!s.config || s.config->size() != roles.size()) {
		error = "The camera does not support the required stream configuration.";
		return false;
	}

	StreamConfiguration &rawCfg = s.config->at(0);
	// Unpacked 12-bit raw at the sensor's full resolution. The Pi 5 can't
	// deliver 12-bit unpacked and substitutes 16-bit (see rawShift); the
	// DNG is packed to 12 bits in software either way.
	rawCfg.pixelFormat = formats::SRGGB12;
	rawCfg.bufferCount = kBufferCount;
	// Pin the sensor's 12-bit full-frame mode explicitly, whatever raw
	// format the pipeline ends up choosing for the stream.
	s.config->sensorConfig = SensorConfiguration();
	s.config->sensorConfig->bitDepth = 12;
	s.config->sensorConfig->outputSize = rawCfg.size;

	StreamConfiguration *mainCfg = nullptr;
	if (format == CaptureFormat::Jpg) {
		mainCfg = &s.config->at(1);
		mainCfg->pixelFormat = formats::YUV420;
		mainCfg->size = rawCfg.size;
		mainCfg->bufferCount = kBufferCount;
	}
	StreamConfiguration &previewCfg = s.config->at(roles.size() - 1);
	previewCfg.pixelFormat = formats::YUV420;
	previewCfg.size = Size(kPreviewWidth, kPreviewHeight);
	previewCfg.bufferCount = kBufferCount;

	if (s.config->validate() == CameraConfiguration::Invalid) {
		error = "The camera rejected the stream configuration.";
		s.config.reset();
		return false;
	}

	// validate() may adjust things (notably the Bayer order, which depends
	// on the sensor's flip state, and the Pi 5 turning 12-bit into 16-bit)
	// - read back what we actually got.
	const std::string rawName = rawCfg.pixelFormat.toString();
	if (rawName.size() != 7 || rawName[0] != 'S' ||
	    (rawName.compare(5, 2, "12") != 0 && rawName.compare(5, 2, "16") != 0)) {
		error = "Unexpected raw pixel format \"" + rawName +
			"\" (need unpacked 12- or 16-bit Bayer).";
		s.config.reset();
		return false;
	}
	s.rawShift = rawName.compare(5, 2, "16") == 0 ? 4 : 0;
	for (int i = 0; i < 4; ++i) {
		switch (rawName[1 + i]) {
		case 'R': s.cfa[i] = 0; break;
		case 'G': s.cfa[i] = 1; break;
		case 'B': s.cfa[i] = 2; break;
		default:
			error = "Unexpected raw Bayer order \"" + rawName + "\".";
			s.config.reset();
			return false;
		}
	}
	if (previewCfg.pixelFormat != formats::YUV420 ||
	    (mainCfg && mainCfg->pixelFormat != formats::YUV420)) {
		error = "The camera changed the processed stream format away from YUV420.";
		s.config.reset();
		return false;
	}

	if (s.camera->configure(s.config.get()) != 0) {
		error = "Failed to configure the camera.";
		s.config.reset();
		return false;
	}

	s.rawStream = rawCfg.stream();
	s.rawWidth = rawCfg.size.width;
	s.rawHeight = rawCfg.size.height;
	s.rawStride = rawCfg.stride;
	s.previewStream = previewCfg.stream();
	s.previewWidth = previewCfg.size.width;
	s.previewHeight = previewCfg.size.height;
	s.previewStride = previewCfg.stride;
	s.mainStream = mainCfg ? mainCfg->stream() : nullptr;
	s.mainYStride = mainCfg ? mainCfg->stride : 0;
	s.format = format;
	s.onPreview = std::move(onPreview);

	auto cleanup = [&] {
		s.requests.clear();
		s.allocator.reset();
		s.unmapAll();
		s.config.reset();
	};

	s.allocator = std::make_unique<FrameBufferAllocator>(s.camera);
	size_t requestCount = SIZE_MAX;
	for (auto &cfg : *s.config) {
		if (s.allocator->allocate(cfg.stream()) < 0) {
			error = "Failed to allocate camera buffers (out of CMA memory?).";
			cleanup();
			return false;
		}
		requestCount = std::min(requestCount, s.allocator->buffers(cfg.stream()).size());
	}
	for (size_t i = 0; i < requestCount; ++i) {
		std::unique_ptr<Request> request = s.camera->createRequest();
		if (!request) {
			error = "Failed to create a camera request.";
			cleanup();
			return false;
		}
		for (auto &cfg : *s.config) {
			FrameBuffer *buffer = s.allocator->buffers(cfg.stream())[i].get();
			if (request->addBuffer(cfg.stream(), buffer) != 0 || !s.mapBuffer(buffer)) {
				error = "Failed to attach/map a camera buffer.";
				cleanup();
				return false;
			}
		}
		s.requests.push_back(std::move(request));
	}

	s.streaming = true;
	s.camera->requestCompleted.connect(&s, &Impl::onRequestCompleted);

	ControlList startControls(s.camera->controls());
	s.fillControls(startControls);
	if (s.camera->start(&startControls) != 0) {
		s.streaming = false;
		s.camera->requestCompleted.disconnect(&s, &Impl::onRequestCompleted);
		error = "Failed to start the camera.";
		cleanup();
		return false;
	}
	for (auto &request : s.requests) {
		s.fillControls(request->controls());
		s.camera->queueRequest(request.get());
	}
	return true;
}

void HqCamera::stop() {
	Impl &s = *impl_;
	std::lock_guard<std::mutex> lifecycle(s.lifecycleMutex);
	if (!s.streaming)
		return;

	s.streaming = false;
	s.camera->stop();
	s.camera->requestCompleted.disconnect(&s, &Impl::onRequestCompleted);

	{
		std::lock_guard<std::mutex> lock(s.stillMutex);
		if (s.pendingStill) {
			s.pendingStill->promise.set_value(std::nullopt);
			s.pendingStill.reset();
		}
	}

	s.requests.clear();
	s.allocator.reset();
	s.unmapAll();
	s.config.reset();
	s.rawStream = s.previewStream = s.mainStream = nullptr;
	s.onPreview = nullptr;
}

void HqCamera::setControls(const CameraControls &requested) {
	Impl &s = *impl_;
	const CameraControlRanges &r = s.ranges;
	CameraControls c = requested;
	c.exposureUs = std::clamp(c.exposureUs, r.exposureMinUs, r.exposureMaxUs);
	c.redGain = std::clamp(c.redGain, r.gainMin, r.gainMax);
	c.blueGain = std::clamp(c.blueGain, r.gainMin, r.gainMax);
	c.brightness = std::clamp(c.brightness, r.brightnessMin, r.brightnessMax);
	c.contrast = std::clamp(c.contrast, r.contrastMin, r.contrastMax);
	c.saturation = std::clamp(c.saturation, r.saturationMin, r.saturationMax);

	std::lock_guard<std::mutex> lock(s.controlsMutex);
	if (c.exposureUs != s.controls.exposureUs || c.redGain != s.controls.redGain ||
	    c.blueGain != s.controls.blueGain || c.brightness != s.controls.brightness ||
	    c.contrast != s.controls.contrast || c.saturation != s.controls.saturation) {
		s.controls = c;
		s.controlsChangedNs = nowNs();
	}
}

void HqCamera::setHistogramEnabled(bool enabled) { impl_->histogramEnabled = enabled; }

void HqCamera::setZoom(std::optional<std::pair<int, int>> sensorCenter) {
	std::lock_guard<std::mutex> lock(impl_->zoomMutex);
	impl_->zoom = sensorCenter;
}

std::optional<StillCapture> HqCamera::captureStill(int timeoutMs) {
	Impl &s = *impl_;
	if (!s.streaming)
		return std::nullopt;

	auto pending = std::make_shared<PendingStill>();
	{
		std::lock_guard<std::mutex> lock(s.controlsMutex);
		pending->expected = s.controls;
		pending->notBeforeNs = std::max(nowNs(), s.controlsChangedNs + kControlSettleNs);
	}
	std::future<std::optional<StillCapture>> future = pending->promise.get_future();
	{
		std::lock_guard<std::mutex> lock(s.stillMutex);
		if (s.pendingStill)
			s.pendingStill->promise.set_value(std::nullopt); // superseded
		s.pendingStill = pending;
	}

	if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
		bool withdrawn = false;
		{
			std::lock_guard<std::mutex> lock(s.stillMutex);
			if (s.pendingStill == pending) {
				s.pendingStill.reset();
				withdrawn = true;
			}
		}
		if (withdrawn)
			return std::nullopt;
		// Otherwise the camera thread already claimed it just as we timed
		// out; the result is being produced and arrives momentarily.
	}
	return future.get();
}

} // namespace hqcore
