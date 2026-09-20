#include "CaptureWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>

#include "../core/AppPaths.h"
#include "../core/DngWriter.h"
#include "../core/ExportSettings.h"
#include "../core/FrameIndex.h"
#include "../core/JpegWriter.h"
#include "../core/Preferences.h"
#include "../core/SpeedAdapter.h"
#include "ClickableLabel.h"
#include "HistogramWidget.h"
#include "OtherSettingsDialog.h"

using hqcore::CameraControls;
using hqcore::CaptureFormat;
using hqcore::HqCamera;
using hqcore::LightController;

namespace {
constexpr int kHistogramAutoOffMs = 60 * 60 * 1000; // 1 hour
// Config files live in ~/.config/c-gugusse (see core/AppPaths.h); main()
// seeds any missing one from the installed defaults before this window opens.
const std::string kSettingsPath = hqcore::configFile("hq-camera-settings.json");
const std::string kPreferencesPath = hqcore::configFile("preferences.json");
const std::string kHardwareConfigPath = hqcore::configFile("hardwarecfg.json");
const std::string kFtpConfigPath = hqcore::configFile("ftp.json");
constexpr char kShmDir[] = "/dev/shm";
constexpr char kShmCompleteDir[] = "/dev/shm/complete";
const QString kCcwIconPath = QString::fromStdString(hqcore::assetFile("icons/ccw.png"));
const QString kCwIconPath = QString::fromStdString(hqcore::assetFile("icons/cw.png"));
const QString kPowerOnIconPath = QString::fromStdString(hqcore::assetFile("icons/powerOn.png"));
const QString kPowerOffIconPath = QString::fromStdString(hqcore::assetFile("icons/powerOff.png"));

// Longest exposure offered by the slider. The camera itself accepts more
// on paper, but longer than this makes no sense for a backlit film frame
// and would wreck the slider's resolution at the short end.
constexpr int kMaxSliderExposureUs = 100000;
constexpr int kJpegQuality = 95;

// Per-motor sensor indicator dot - polled rather than event-driven (see
// onSensorPollTimer()), 20Hz is far more than a human needs to perceive it
// as live.
constexpr int kSensorPollMs = 50;
constexpr char kSensorIndicatorOffStyle[] =
	"background-color: #444; border: 1px solid #222; border-radius: 7px;";
constexpr char kSensorIndicatorOnStyle[] =
	"background-color: #2ecc71; border: 1px solid #1e8449; border-radius: 7px;";

// Used only as the initial dropdown selection when no film format has
// been saved to preferences.json yet (or the saved one no longer exists
// in hardwarecfg.json) - see the constructor's film-format setup.
constexpr char kDefaultFilmFormatName[] = "35mm";
constexpr long kSequenceSettleDelayMs = 100; // step F
constexpr const char *kMotorNames[3] = {"feeder", "filmdrive", "pickup"};

constexpr const char *kCaptureFormatNames[2] = {"DNG", "JPG"};
constexpr const char *kCaptureFormatSuffixes[2] = {"dng", "jpg"};

// Numpad motor jog/toggle shortcuts (see CaptureWindow::keyPressEvent):
// numpad 7/8/9 sit above 4/5/6 above 1/2/3, mirroring feeder/filmdrive/
// pickup's top-to-bottom row order in motorRows_ (MotorFeeder=0,
// MotorFilmdrive=1, MotorPickup=2 - if that enum's order ever changes,
// update motorIndex below to match), and left/middle/right within each
// row mirrors ccw/toggle/cw.
enum class MotorKeyAction { Ccw, Toggle, Cw };
struct MotorKeyMapping {
	int motorIndex;
	MotorKeyAction action;
};

std::optional<MotorKeyMapping> motorKeyMappingFor(int key) {
	switch (key) {
	case Qt::Key_7:
		return MotorKeyMapping{0, MotorKeyAction::Ccw};
	case Qt::Key_8:
		return MotorKeyMapping{0, MotorKeyAction::Toggle};
	case Qt::Key_9:
		return MotorKeyMapping{0, MotorKeyAction::Cw};
	case Qt::Key_4:
		return MotorKeyMapping{1, MotorKeyAction::Ccw};
	case Qt::Key_5:
		return MotorKeyMapping{1, MotorKeyAction::Toggle};
	case Qt::Key_6:
		return MotorKeyMapping{1, MotorKeyAction::Cw};
	case Qt::Key_1:
		return MotorKeyMapping{2, MotorKeyAction::Ccw};
	case Qt::Key_2:
		return MotorKeyMapping{2, MotorKeyAction::Toggle};
	case Qt::Key_3:
		return MotorKeyMapping{2, MotorKeyAction::Cw};
	default:
		return std::nullopt;
	}
}

// filmdrive's D+E split means D always advances exactly ignoreInitial
// steps by construction, so a "short fault" for filmdrive can't compare
// E's own step count against the full ignoreInitial (it would never
// fire) - it compares against this much smaller cutoff instead, matching
// GugusseRoller's real intent: the hole appearing essentially the
// instant the blind phase ends (E contributing ~0 extra steps) is what's
// actually suspicious, not "fewer than ignoreInitial total".
constexpr long kFilmdriveShortFaultSteps = 2;

constexpr int kMaxPendingUploads = 4;
constexpr double kUploadBacklogFaultSeconds = 600.0; // 10 minutes
constexpr int kUploadBacklogPollMs = 2000;

int formatIndex(CaptureFormat f) { return f == CaptureFormat::Dng ? 0 : 1; }
} // namespace

CaptureWindow::CaptureWindow(QWidget *parent) : QMainWindow(parent) {
	qRegisterMetaType<QVector<quint32>>();

	auto *central = new QWidget(this);
	auto *layout = new QVBoxLayout(central);

	auto *topRow = new QHBoxLayout();
	lightButton_ = new QPushButton("White light", central);
	lightButton_->setCheckable(true);
	topRow->addWidget(lightButton_);
	topRow->addSpacing(16);
	topRow->addWidget(new QLabel("Save as:", central));
	captureFormatCombo_ = new QComboBox(central);
	captureFormatCombo_->addItems({kCaptureFormatNames[0], kCaptureFormatNames[1]});
	captureFormatCombo_->setToolTip(
		"DNG: 12-bit raw sensor data. JPG: processed by the camera's ISP\n"
		"(white balance, brightness, contrast, saturation applied).");
	topRow->addWidget(captureFormatCombo_);
	topRow->addStretch(1);
	histogramToggleButton_ = new QPushButton("Histogram", central);
	histogramToggleButton_->setCheckable(true);
	topRow->addWidget(histogramToggleButton_);
	saveSettingsButton_ = new QPushButton("Save Settings", central);
	topRow->addWidget(saveSettingsButton_);
	layout->addLayout(topRow);

	auto *projectRow = new QHBoxLayout();
	projectEdit_ = new QLineEdit(central);
	projectEdit_->setText(QDateTime::currentDateTime().toString("yyyyMMdd-HHmm"));
	projectRow->addWidget(new QLabel("Project:", central));
	projectRow->addWidget(projectEdit_, /*stretch=*/1);
	projectRow->addSpacing(16);
	projectRow->addWidget(new QLabel("Film format:", central));
	filmFormatCombo_ = new QComboBox(central);
	projectRow->addWidget(filmFormatCombo_);
	otherSettingsButton_ = new QPushButton("Other settings...", central);
	otherSettingsButton_->setToolTip(
		"Export mode (FTP upload / local copy), FTP server, motor directions.");
	projectRow->addWidget(otherSettingsButton_);
	layout->addLayout(projectRow);

	// Camera controls, two per row: slider integer position / scale =
	// control value. Columns: name, slider, value | gap | name, slider, value.
	auto *controlsGrid = new QGridLayout();
	auto addControl = [&](ControlSlider &control, const char *name, double scale, int row,
			      int firstColumn) {
		control.slider = new QSlider(Qt::Horizontal, central);
		control.label = new QLabel("--", central);
		control.label->setMinimumWidth(firstColumn == 0 ? 130 : 50);
		control.scale = scale;
		controlsGrid->addWidget(new QLabel(name, central), row, firstColumn);
		controlsGrid->addWidget(control.slider, row, firstColumn + 1);
		controlsGrid->addWidget(control.label, row, firstColumn + 2);
	};
	addControl(exposureControl_, "Exposure:", 1.0, 0, 0);
	addControl(brightnessControl_, "Brightness:", 100.0, 0, 4);
	addControl(contrastControl_, "Contrast:", 100.0, 1, 0);
	addControl(saturationControl_, "Saturation:", 100.0, 1, 4);
	addControl(redGainControl_, "Red gain:", 100.0, 2, 0);
	addControl(blueGainControl_, "Blue gain:", 100.0, 2, 4);
	controlsGrid->setColumnStretch(1, 1);
	controlsGrid->setColumnStretch(5, 1);
	controlsGrid->setColumnMinimumWidth(3, 24); // gap between the two halves
	layout->addLayout(controlsGrid);

	previewLabel_ = new ClickableLabel(central);
	previewLabel_->setAlignment(Qt::AlignCenter);
	// The preview takes whatever space the other rows leave: the pixmap is
	// re-scaled to the label on every frame, and Ignored stops the pixmap's
	// own size from feeding back into the layout (which would make the
	// window unable to shrink). The small minimum just keeps it usable.
	previewLabel_->setMinimumSize(320, 240);
	previewLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	previewLabel_->setStyleSheet("background-color: black;");
	layout->addWidget(previewLabel_, /*stretch=*/1);

	histogramWidget_ = new HistogramWidget(central);
	// Fixed height, so showing/hiding it takes exactly that much from the
	// preview (the only stretchy widget) and never resizes the window.
	histogramWidget_->setFixedHeight(140);
	histogramWidget_->hide();
	layout->addWidget(histogramWidget_);

	setUpMotors(central, layout);

	auto *bottomRow = new QHBoxLayout();
	statusLabel_ = new QLabel(central);
	QFont statusFont = statusLabel_->font();
	statusFont.setPointSize(statusFont.pointSize() + 2);
	statusLabel_->setFont(statusFont);
	captureFrameButton_ = new QPushButton("Capture Frame", central);
	captureFrameButton_->setMinimumWidth(180);
	captureFrameButton_->setAutoDefault(true);
	captureFrameButton_->setDefault(true);
	sequenceButton_ = new QPushButton("Start Sequence", central);
	sequenceButton_->setMinimumWidth(160);
	emergencyStopButton_ = new QPushButton("EMERGENCY STOP", central);
	emergencyStopButton_->setMinimumWidth(160);
	emergencyStopButton_->setStyleSheet(
		"background-color: #c0392b; color: white; font-weight: bold;");
	bottomRow->addWidget(statusLabel_, /*stretch=*/1);
	bottomRow->addWidget(captureFrameButton_);
	bottomRow->addWidget(sequenceButton_);
	bottomRow->addWidget(emergencyStopButton_);
	layout->addLayout(bottomRow);

	setCentralWidget(central);
	setWindowTitle("Raspberry Pi HQ Camera Capture");

	histogramAutoOffTimer_ = new QTimer(this);
	histogramAutoOffTimer_->setSingleShot(true);
	histogramAutoOffTimer_->setInterval(kHistogramAutoOffMs);

	connect(this, &CaptureWindow::frameReady, this, &CaptureWindow::onFrameReady);
	for (const ControlSlider *c :
	     {&exposureControl_, &redGainControl_, &blueGainControl_, &brightnessControl_,
	      &contrastControl_, &saturationControl_}) {
		connect(c->slider, &QSlider::valueChanged, this, &CaptureWindow::onControlChanged);
	}
	connect(captureFrameButton_, &QPushButton::clicked, this,
		&CaptureWindow::onCaptureFrameClicked);
	connect(this, &CaptureWindow::manualCaptureFinished, this,
		&CaptureWindow::onManualCaptureFinished);
	connect(histogramToggleButton_, &QPushButton::toggled, this,
		&CaptureWindow::onHistogramToggled);
	connect(lightButton_, &QPushButton::toggled, this, &CaptureWindow::onLightToggled);
	connect(previewLabel_, &ClickableLabel::clicked, this, &CaptureWindow::onPreviewClicked);
	connect(saveSettingsButton_, &QPushButton::clicked, this,
		&CaptureWindow::onSaveSettingsClicked);
	connect(projectEdit_, &QLineEdit::editingFinished, this,
		&CaptureWindow::onProjectNameEdited);
	connect(sequenceButton_, &QPushButton::clicked, this,
		&CaptureWindow::onSequenceButtonClicked);
	connect(emergencyStopButton_, &QPushButton::clicked, this,
		&CaptureWindow::onEmergencyStopClicked);
	connect(this, &CaptureWindow::sequenceStatusChanged, this,
		&CaptureWindow::onSequenceStatusChanged);
	connect(this, &CaptureWindow::sequenceFinished, this, &CaptureWindow::onSequenceFinished);
	connect(this, &CaptureWindow::motorSpeedChanged, this, &CaptureWindow::onMotorSpeedChanged);
	connect(filmFormatCombo_, &QComboBox::currentIndexChanged, this,
		&CaptureWindow::onFilmFormatChanged);
	connect(otherSettingsButton_, &QPushButton::clicked, this,
		&CaptureWindow::onOtherSettingsClicked);
	connect(histogramAutoOffTimer_, &QTimer::timeout, this, [this] {
		histogramToggleButton_->setChecked(false);
		statusBar()->showMessage("Histogram auto-disabled after 1 hour to save CPU.", 5000);
	});

	light_ = LightController::open();
	if (!light_) {
		lightButton_->setEnabled(false);
		lightButton_->setToolTip("GPIO light control unavailable (no Pi GPIO chip found).");
	}

	std::filesystem::create_directories(kShmCompleteDir);
	applyExportSettings();

	const auto prefs = hqcore::Preferences::load(kPreferencesPath);

	const auto formatNames = hqcore::HardwareConfig::listFilmFormatNames(kHardwareConfigPath);
	if (formatNames && !formatNames->empty()) {
		for (const auto &name : *formatNames)
			filmFormatCombo_->addItem(QString::fromStdString(name));

		QString initialName;
		if (!prefs.filmFormat.empty() &&
		    filmFormatCombo_->findText(QString::fromStdString(prefs.filmFormat)) >= 0) {
			initialName = QString::fromStdString(prefs.filmFormat);
		} else if (filmFormatCombo_->findText(kDefaultFilmFormatName) >= 0) {
			initialName = kDefaultFilmFormatName;
		} else {
			initialName = filmFormatCombo_->itemText(0);
		}

		// Picking a default/fallback here isn't a "change" needing a
		// save.
		lastSavedFilmFormatName_ = initialName.toStdString();

		filmFormatCombo_->blockSignals(true);
		filmFormatCombo_->setCurrentText(initialName);
		filmFormatCombo_->blockSignals(false);
		applyFilmFormat(initialName);
	} else {
		filmFormatCombo_->setEnabled(false);
		filmFormatCombo_->setToolTip(
			QString("No film formats found in %1.").arg(QString::fromStdString(kHardwareConfigPath)));
		sequenceButton_->setEnabled(false);
		sequenceButton_->setToolTip(
			QString("Sequence unavailable: %1 has no film formats.")
				.arg(QString::fromStdString(kHardwareConfigPath)));
	}

	// Capture format: saved preference, else DNG (the full-quality one).
	{
		int index = 0;
		for (int i = 0; i < 2; ++i) {
			if (prefs.captureFormat == kCaptureFormatNames[i])
				index = i;
		}
		lastSavedCaptureFormat_ = kCaptureFormatNames[index];
		captureFormatCombo_->blockSignals(true);
		captureFormatCombo_->setCurrentIndex(index);
		captureFormatCombo_->blockSignals(false);
	}
	connect(captureFormatCombo_, &QComboBox::currentIndexChanged, this,
		&CaptureWindow::onCaptureFormatChanged);

	sensorPollTimer_ = new QTimer(this);
	sensorPollTimer_->setInterval(kSensorPollMs);
	connect(sensorPollTimer_, &QTimer::timeout, this, &CaptureWindow::onSensorPollTimer);
	sensorPollTimer_->start();

	if (auto error = setUpCamera()) {
		for (const ControlSlider *c :
		     {&exposureControl_, &redGainControl_, &blueGainControl_, &brightnessControl_,
		      &contrastControl_, &saturationControl_}) {
			c->slider->setEnabled(false);
		}
		captureFrameButton_->setEnabled(false);
		sequenceButton_->setEnabled(false);
		captureFormatCombo_->setEnabled(false);
		histogramToggleButton_->setEnabled(false);
		statusLabel_->setText(*error);
		QMessageBox::critical(this, "Camera setup failed", *error);
		return;
	}

	statusLabel_->setText("Ready. Adjust the camera controls, then capture.");
}

CaptureWindow::~CaptureWindow() {
	// Make sure the sequence/capture threads (if any) are stopped and
	// joined before member destruction begins - they capture `this` and
	// touch motorRows_/camera_/etc, all of which must still be alive.
	emergencyStopRequested_ = true;
	gentleStopRequested_ = true;
	if (sequenceThread_.joinable())
		sequenceThread_.join();
	if (manualCaptureThread_.joinable())
		manualCaptureThread_.join();
	if (motorTestThread_.joinable())
		motorTestThread_.join();
	// A start-index lookup (see refreshStartIndex()) posts its result
	// back to `this`.
	QThreadPool::globalInstance()->waitForDone();
	// Stop the camera's callback thread before any widget goes away - it
	// emits signals on `this`.
	if (camera_)
		camera_->stop();
}

std::optional<QString> CaptureWindow::setUpCamera() {
	std::string error;
	camera_ = HqCamera::open(error);
	if (!camera_)
		return QString::fromStdString(error);

	const auto &range = camera_->ranges();
	const auto settings = hqcore::CameraSettings::load(kSettingsPath);

	auto setup = [](ControlSlider &c, double lo, double hi, double initial) {
		c.slider->blockSignals(true);
		c.slider->setRange(static_cast<int>(std::lround(lo * c.scale)),
				   static_cast<int>(std::lround(hi * c.scale)));
		c.slider->setValue(static_cast<int>(
			std::lround(std::clamp(initial, lo, hi) * c.scale)));
		c.slider->blockSignals(false);
	};
	// Slider ranges are the camera's own limits, trimmed to what is useful
	// (a 0-32 gain slider would leave the 1-4 region nearly unusable).
	setup(exposureControl_, range.exposureMinUs,
	      std::min(range.exposureMaxUs, kMaxSliderExposureUs), settings.exposureMicroseconds);
	setup(redGainControl_, std::max(range.gainMin, 0.1f), std::min(range.gainMax, 8.0f),
	      settings.redGain);
	setup(blueGainControl_, std::max(range.gainMin, 0.1f), std::min(range.gainMax, 8.0f),
	      settings.blueGain);
	setup(brightnessControl_, range.brightnessMin, range.brightnessMax, settings.brightness);
	setup(contrastControl_, range.contrastMin, std::min(range.contrastMax, 8.0f),
	      settings.contrast);
	setup(saturationControl_, range.saturationMin, std::min(range.saturationMax, 8.0f),
	      settings.saturation);
	updateControlLabels();
	// What is on the sliders now (possibly clamped into range) is the
	// baseline "saved" state.
	lastSavedSettings_ = currentSettings();
	camera_->setControls(currentControls());

	return startCameraStream(selectedCaptureFormat());
}

std::optional<QString> CaptureWindow::startCameraStream(CaptureFormat format) {
	std::string error;
	const bool ok = camera_->start(
		format,
		[this](hqcore::PreviewFrame frame, const hqcore::Histogram *hist) {
			QImage image(frame.rgb.data(), frame.width, frame.height, frame.width * 3,
				     QImage::Format_RGB888);
			QVector<quint32> bins;
			if (hist) {
				bins.resize(static_cast<int>(hist->bins.size()));
				std::copy(hist->bins.begin(), hist->bins.end(), bins.begin());
			}
			emit frameReady(image.copy(), frame.zoomed, std::move(bins));
		},
		error);
	if (!ok)
		return QString("Failed to start the camera: %1").arg(QString::fromStdString(error));
	previewScale_ = static_cast<double>(camera_->rawWidth()) / HqCamera::kPreviewWidth;
	return std::nullopt;
}

CameraControls CaptureWindow::currentControls() const {
	CameraControls c;
	c.exposureUs = exposureControl_.slider->value();
	c.redGain = static_cast<float>(redGainControl_.slider->value() / redGainControl_.scale);
	c.blueGain = static_cast<float>(blueGainControl_.slider->value() / blueGainControl_.scale);
	c.brightness =
		static_cast<float>(brightnessControl_.slider->value() / brightnessControl_.scale);
	c.contrast = static_cast<float>(contrastControl_.slider->value() / contrastControl_.scale);
	c.saturation =
		static_cast<float>(saturationControl_.slider->value() / saturationControl_.scale);
	return c;
}

hqcore::CameraSettings CaptureWindow::currentSettings() const {
	const CameraControls c = currentControls();
	hqcore::CameraSettings s;
	s.exposureMicroseconds = c.exposureUs;
	s.redGain = c.redGain;
	s.blueGain = c.blueGain;
	s.brightness = c.brightness;
	s.contrast = c.contrast;
	s.saturation = c.saturation;
	return s;
}

CaptureFormat CaptureWindow::selectedCaptureFormat() const {
	return captureFormatCombo_->currentIndex() == 0 ? CaptureFormat::Dng : CaptureFormat::Jpg;
}

void CaptureWindow::updateControlLabels() {
	const CameraControls c = currentControls();
	exposureControl_.label->setText(
		QString("%1 us (%2 ms)").arg(c.exposureUs).arg(c.exposureUs / 1000.0, 0, 'f', 2));
	redGainControl_.label->setText(QString::number(c.redGain, 'f', 2));
	blueGainControl_.label->setText(QString::number(c.blueGain, 'f', 2));
	brightnessControl_.label->setText(QString::number(c.brightness, 'f', 2));
	contrastControl_.label->setText(QString::number(c.contrast, 'f', 2));
	saturationControl_.label->setText(QString::number(c.saturation, 'f', 2));
}

void CaptureWindow::onControlChanged() {
	updateControlLabels();
	if (camera_)
		camera_->setControls(currentControls());
}

void CaptureWindow::onCaptureFormatChanged(int) {
	if (!camera_)
		return;
	// The stream set differs per format (JPG adds a full-resolution
	// processed stream), so switching means restarting the camera.
	const bool wasZoomed = zoomed_;
	zoomed_ = false;
	camera_->setZoom(std::nullopt);
	statusLabel_->setText("Switching capture format...");
	if (auto error = startCameraStream(selectedCaptureFormat())) {
		statusLabel_->setText(*error);
		QMessageBox::critical(this, "Camera restart failed", *error);
		captureFrameButton_->setEnabled(false);
		sequenceButton_->setEnabled(false);
		return;
	}
	Q_UNUSED(wasZoomed);
	statusLabel_->setText(QString("Saving as %1.").arg(
		kCaptureFormatNames[formatIndex(selectedCaptureFormat())]));
}

void CaptureWindow::stopCameraForFault() {
	// HqCamera::stop() blocks briefly - a GUI-thread stall here is accepted
	// since this only runs right as a fault is reported, not on any hot
	// path.
	if (camera_)
		camera_->stop();
	previewLabel_->clear();
	previewLabel_->setText(
		"Camera stopped: sequence fault.\nAcknowledge the fault dialog to resume.");
	previewLabel_->setStyleSheet("background-color: black; color: #e74c3c;");
	captureFrameButton_->setEnabled(false);
	sequenceButton_->setEnabled(false);
}

void CaptureWindow::resumeCameraAfterFaultAcknowledged() {
	previewLabel_->setStyleSheet("background-color: black;");
	if (auto error = startCameraStream(selectedCaptureFormat())) {
		previewLabel_->setText("Camera restart failed - restart the application.");
		QMessageBox::critical(this, "Camera restart failed",
				      *error + "\nRestart the application.");
		return; // leave the capture buttons disabled
	}
	captureFrameButton_->setEnabled(true);
	sequenceButton_->setEnabled(filmFormat_.has_value());
}

void CaptureWindow::onFrameReady(QImage image, bool zoomed, QVector<quint32> histogram) {
	// The zoom view is one displayed pixel per Bayer quad; show it at 2x so
	// sensor pixels appear at their true size.
	if (zoomed) {
		previewLabel_->setPixmap(QPixmap::fromImage(image).scaled(
			(image.size() * 2).boundedTo(previewLabel_->size()), Qt::KeepAspectRatio,
			Qt::FastTransformation));
	} else {
		previewLabel_->setPixmap(QPixmap::fromImage(image).scaled(
			previewLabel_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
	}

	if (!histogram.isEmpty() && histogramToggleButton_->isChecked())
		histogramWidget_->setData(histogram);
}

void CaptureWindow::onHistogramToggled(bool enabled) {
	histogramWidget_->setVisible(enabled);
	if (camera_)
		camera_->setHistogramEnabled(enabled);
	if (enabled)
		histogramAutoOffTimer_->start();
	else
		histogramAutoOffTimer_->stop();
}

void CaptureWindow::onLightToggled(bool on) {
	if (light_)
		light_->set(on ? LightController::Color::White : LightController::Color::Off);
}

void CaptureWindow::onPreviewClicked(QPoint pos) {
	if (!camera_)
		return;

	if (zoomed_) {
		zoomed_ = false;
		camera_->setZoom(std::nullopt);
		return;
	}

	const QPixmap pix = previewLabel_->pixmap();
	if (pix.isNull())
		return;
	const int offsetX = (previewLabel_->width() - pix.width()) / 2;
	const int offsetY = (previewLabel_->height() - pix.height()) / 2;
	const double imgX = pos.x() - offsetX;
	const double imgY = pos.y() - offsetY;
	if (imgX < 0 || imgY < 0 || imgX >= pix.width() || imgY >= pix.height())
		return;

	// The pixmap on screen is the preview scaled to fit the label; map back
	// to preview pixels, then to sensor pixels.
	const double toPreview = static_cast<double>(HqCamera::kPreviewWidth) / pix.width();
	zoomed_ = true;
	camera_->setZoom(std::make_pair(static_cast<int>(imgX * toPreview * previewScale_),
					static_cast<int>(imgY * toPreview * previewScale_)));
}

void CaptureWindow::onSaveSettingsClicked() {
	const hqcore::CameraSettings current = currentSettings();
	const std::string currentFilmFormat =
		filmFormatCombo_ ? filmFormatCombo_->currentText().toStdString() : std::string();
	const std::string currentCaptureFormat =
		kCaptureFormatNames[formatIndex(selectedCaptureFormat())];

	const bool cameraChanged = !(current == lastSavedSettings_);
	const bool prefsChanged = currentFilmFormat != lastSavedFilmFormatName_ ||
				  currentCaptureFormat != lastSavedCaptureFormat_;
	if (!cameraChanged && !prefsChanged) {
		statusBar()->showMessage("Nothing to save.", 3000);
		return;
	}

	bool ok = true;

	if (cameraChanged) {
		if (current.save(kSettingsPath))
			lastSavedSettings_ = current;
		else
			ok = false;
	}

	if (prefsChanged) {
		hqcore::Preferences prefs;
		prefs.filmFormat = currentFilmFormat;
		prefs.captureFormat = currentCaptureFormat;
		if (prefs.save(kPreferencesPath)) {
			lastSavedFilmFormatName_ = currentFilmFormat;
			lastSavedCaptureFormat_ = currentCaptureFormat;
		} else {
			ok = false;
		}
	}

	statusBar()->showMessage(ok ? "Settings saved." : "Failed to save settings.",
				  ok ? 3000 : 5000);
}

bool CaptureWindow::hasUnsavedSettings() const {
	if (!camera_)
		return false;
	if (!(currentSettings() == lastSavedSettings_))
		return true;
	if (filmFormatCombo_ &&
	    filmFormatCombo_->currentText().toStdString() != lastSavedFilmFormatName_) {
		return true;
	}
	return kCaptureFormatNames[formatIndex(selectedCaptureFormat())] != lastSavedCaptureFormat_;
}

void CaptureWindow::closeEvent(QCloseEvent *event) {
	if (!hasUnsavedSettings()) {
		event->accept();
		return;
	}

	const auto choice = QMessageBox::warning(
		this, "Unsaved settings",
		"Camera, film format or capture format settings have changed but haven't "
		"been saved. Save before exiting?",
		QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
	if (choice == QMessageBox::Cancel) {
		event->ignore();
		return;
	}
	if (choice == QMessageBox::Save)
		onSaveSettingsClicked();
	event->accept();
}

bool CaptureWindow::captureAndStage(std::atomic<bool> *abort, QString &fileName) {
	std::lock_guard<std::mutex> lock(captureMutex_);
	if (!camera_ || (abort && *abort))
		return false;

	auto still = camera_->captureStill();
	if (!still)
		return false;

	const int index = still->format == CaptureFormat::Dng ? 0 : 1;
	const int frameNumber = frameCount_.load();
	fileName = QString("%1.%2")
			   .arg(frameNumber, 5, 10, QChar('0'))
			   .arg(kCaptureFormatSuffixes[index]);
	// Write to /dev/shm/<NNNNN>.<ext>, then rename into
	// /dev/shm/complete/ once the write is done - the rename is atomic
	// (same tmpfs), so FtpUploader (watching the complete/ dir) never
	// sees a partially-written file. Matches ../GugusseRoller's
	// captureCycle().
	const std::string tempPath = QString("%1/%2").arg(kShmDir, fileName).toStdString();
	const std::string finalPath = QString("%1/%2").arg(kShmCompleteDir, fileName).toStdString();

	bool ok;
	if (still->format == CaptureFormat::Dng) {
		hqcore::DngMetadata meta;
		meta.cfaPattern = still->cfaPattern;
		meta.blackLevel = still->blackLevel;
		meta.whiteLevel = still->whiteLevel;
		meta.redGain = still->redGain;
		meta.blueGain = still->blueGain;
		meta.hasCcm = still->hasCcm;
		meta.ccm = still->ccm;
		meta.description = QString("exposure %1us, red gain %2, blue gain %3")
					   .arg(still->exposureUs)
					   .arg(still->redGain, 0, 'f', 3)
					   .arg(still->blueGain, 0, 'f', 3)
					   .toStdString();
		ok = hqcore::writeDng(tempPath, still->width, still->height, still->raw, meta);
	} else {
		ok = hqcore::writeJpegFromYuv420(tempPath, still->width, still->height,
						      still->y.data(), still->yStride,
						      still->u.data(), still->v.data(),
						      still->uvStride, kJpegQuality);
	}

	if (ok && std::rename(tempPath.c_str(), finalPath.c_str()) != 0)
		ok = false;
	// Advance only if refreshStartIndex() didn't re-seed the counter
	// (project name changed) while this frame was being written.
	if (ok) {
		int expected = frameNumber;
		frameCount_.compare_exchange_strong(expected, frameNumber + 1);
	}
	return ok;
}

void CaptureWindow::onCaptureFrameClicked() {
	if (!camera_ || manualCaptureRunning_)
		return;
	if (indexLookupPending_) {
		statusLabel_->setText("Checking the project directory for existing frames - "
				      "try again in a moment.");
		return;
	}
	if (manualCaptureThread_.joinable())
		manualCaptureThread_.join();

	captureFrameButton_->setEnabled(false);
	captureFormatCombo_->setEnabled(false);
	sequenceButton_->setEnabled(false);
	statusLabel_->setText("Capturing...");
	manualCaptureRunning_ = true;

	manualCaptureThread_ = std::thread([this] {
		// Light the film for the capture if the operator hasn't already;
		// restore the previous state afterwards. (captureStill() only
		// accepts frames exposed after this point, so the LED is on for
		// the whole exposure.)
		const bool lightWasOn = light_ && light_->current() != LightController::Color::Off;
		if (light_ && !lightWasOn)
			light_->set(LightController::Color::White);

		QString fileName;
		const bool ok = captureAndStage(nullptr, fileName);

		if (light_ && !lightWasOn)
			light_->set(LightController::Color::Off);
		emit manualCaptureFinished(
			ok, ok ? QString("Saved: %1/%2").arg(kShmCompleteDir, fileName)
			       : QString("Capture failed."));
	});
}

void CaptureWindow::onManualCaptureFinished(bool ok, QString message) {
	manualCaptureRunning_ = false;
	statusLabel_->setText(message);
	if (!ok)
		QMessageBox::critical(this, "Capture failed", message);
	if (!sequenceRunning_) {
		captureFrameButton_->setEnabled(true);
		captureFormatCombo_->setEnabled(true);
		sequenceButton_->setEnabled(filmFormat_.has_value());
	}
}

void CaptureWindow::onProjectNameEdited() {
	if (ftpUploader_)
		ftpUploader_->setProjectName(projectEdit_->text().toStdString());
	if (localMover_)
		localMover_->setProjectName(projectEdit_->text().toStdString());
	// editingFinished also fires on plain focus-out - only a real change
	// of project needs its directory checked.
	if (projectEdit_->text().toStdString() != lastIndexedProject_)
		refreshStartIndex();
}

void CaptureWindow::refreshStartIndex() {
	const std::string project = projectEdit_->text().toStdString();
	lastIndexedProject_ = project;
	const uint64_t generation = ++indexLookupGeneration_;

	// Same source of truth as applyExportSettings(): the destination is
	// whatever the active exporter writes to. With no usable exporter
	// nothing lands anywhere, so there's nothing to continue from.
	const auto settings = hqcore::ExportSettings::load(kHardwareConfigPath);
	const bool local = settings.mode == hqcore::ExportSettings::Mode::Local;
	std::optional<hqcore::FtpConfig> ftp;
	if (!local)
		ftp = hqcore::FtpConfig::load(kFtpConfigPath);
	if (!local && !ftp) {
		indexLookupPending_ = false;
		return;
	}

	indexLookupPending_ = true;
	statusBar()->showMessage("Checking the project directory for existing frames...");

	const std::vector<std::string> extensions(std::begin(kCaptureFormatSuffixes),
						  std::end(kCaptureFormatSuffixes));
	const std::string destination =
		local ? settings.localPath + "/" + project : ftp->baseUrl() + "/" + project;
	// A network listing can take a while when the server is unreachable,
	// hence off the GUI thread; captures are held until it reports (see
	// indexLookupPending_).
	QThreadPool::globalInstance()->start([this, generation, local, settings, ftp, project,
					      extensions, destination] {
		const hqcore::IndexLookup lookup =
			local ? hqcore::lookupNextIndexLocal(settings.localPath, project,
							     kShmCompleteDir, extensions)
			      : hqcore::lookupNextIndexFtp(*ftp, project, kShmCompleteDir, extensions);
		QMetaObject::invokeMethod(
			this,
			[this, generation, lookup, destination] {
				if (generation != indexLookupGeneration_)
					return; // the project changed again - a newer lookup follows
				indexLookupPending_ = false;
				const QString where = QString::fromStdString(destination);
				if (lookup.ok) {
					frameCount_ = lookup.next;
					statusBar()->showMessage(
						lookup.next == 0
							? QString("%1: %2 - starting at frame 0.")
								  .arg(where,
								       lookup.existed ? "no frames yet"
										      : "new project")
							: QString("%1 already holds frames up to %2 - "
								  "continuing at frame %3.")
								  .arg(where)
								  .arg(lookup.next - 1)
								  .arg(lookup.next),
						8000);
					return;
				}
				statusBar()->clearMessage();
				QMessageBox::warning(
					this, "Could not check the project directory",
					QString("Could not check %1 for existing frames:\n%2\n\n"
						"Numbering continues at frame %3, which may overwrite "
						"frames already there. To check again, fix the problem "
						"and re-enter the project name, or save the \"Other "
						"settings\".")
						.arg(where,
						     QString::fromStdString(lookup.error))
						.arg(frameCount_.load()));
			},
			Qt::QueuedConnection);
	});
}

void CaptureWindow::applyExportSettings() {
	// Only one exporter may drain /dev/shm/complete, and destroying the
	// old one first also lets a transfer in flight finish.
	ftpUploader_.reset();
	localMover_.reset();

	const auto settings = hqcore::ExportSettings::load(kHardwareConfigPath);
	const std::string project = projectEdit_->text().toStdString();
	if (settings.mode == hqcore::ExportSettings::Mode::Local) {
		localMover_.emplace(settings.localPath, kShmCompleteDir, project);
		projectEdit_->setToolTip(QString("Frames are moved to %1/<project> (local copy).")
						 .arg(QString::fromStdString(settings.localPath)));
	} else if (auto ftpConfig = hqcore::FtpConfig::load(kFtpConfigPath)) {
		ftpUploader_.emplace(std::move(*ftpConfig), kShmCompleteDir, project);
		projectEdit_->setToolTip("Frames are uploaded into a directory of this name on "
					  "the FTP server.");
	} else {
		projectEdit_->setToolTip(
			QString("FTP upload unavailable (%1 missing or invalid) - "
				"captures still save to %2. See \"Other settings...\".")
				.arg(QString::fromStdString(kFtpConfigPath), kShmCompleteDir));
	}

	// A different destination (or first start) - continue its numbering.
	refreshStartIndex();
}

void CaptureWindow::runMotorTest(int motorIndex, bool invert, std::function<void()> done) {
	if (motorIndex < 0 || motorIndex >= static_cast<int>(motorRows_.size()) ||
	    !motorRows_[motorIndex].motor) {
		QMetaObject::invokeMethod(this, std::move(done), Qt::QueuedConnection);
		return;
	}
	if (motorTestThread_.joinable())
		motorTestThread_.join();

	motorTestThread_ = std::thread([this, motorIndex, invert, done = std::move(done)] {
		hqcore::Motor &motor = *motorRows_[motorIndex].motor;
		const bool oldInvert = motor.config().invert;
		const bool wasEnabled = motor.isEnabled();

		// About 1000 steps at up to 1000 steps/s (as in
		// ../GugusseRoller's blind test move), gently ramped by
		// advanceFixedSteps(). Always "clockwise": the operator judges
		// the physical direction and ticks "invert" accordingly.
		const double speed = std::min(motor.config().maxSpeed, 1000.0);
		const double speed2 = std::min(speed, std::max(motor.config().minSpeed, speed / 5.0));
		std::atomic<bool> noAbort{false};

		motor.setInvert(invert);
		if (!wasEnabled)
			motor.enable();
		motor.advanceFixedSteps(hqcore::MotorDirection::Cw, 1000, speed, speed2, noAbort);
		if (!wasEnabled)
			motor.disable();
		motor.setInvert(oldInvert);

		QMetaObject::invokeMethod(this, done, Qt::QueuedConnection);
	});
}

void CaptureWindow::onOtherSettingsClicked() {
	if (sequenceRunning_)
		return;

	OtherSettingsDialog::Values initial;
	initial.exportSettings = hqcore::ExportSettings::load(kHardwareConfigPath);
	initial.ftp = hqcore::FtpConfig::loadForEditing(kFtpConfigPath);
	// The direction flags shown come from the file rather than the open
	// Motor objects, so a motor that failed to open (no GPIO) still
	// round-trips its value untouched.
	const auto hwConfig = hqcore::HardwareConfig::load(kHardwareConfigPath);
	std::array<bool, 3> motorAvailable{};
	if (hwConfig) {
		initial.invert = {hwConfig->feeder.invert, hwConfig->filmdrive.invert,
				  hwConfig->pickup.invert};
		for (int i = 0; i < 3; ++i)
			motorAvailable[i] = motorRows_[i].motor.has_value();
	}

	OtherSettingsDialog dialog(
		initial, motorAvailable,
		[this](int motor, bool invert, std::function<void()> done) {
			runMotorTest(motor, invert, std::move(done));
		},
		this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const auto values = dialog.values();
	QStringList failures;
	if (!values.ftp.save(kFtpConfigPath))
		failures << QString("Could not write %1.").arg(QString::fromStdString(kFtpConfigPath));
	if (!values.exportSettings.save(kHardwareConfigPath))
		failures << QString("Could not write the export mode to %1.").arg(QString::fromStdString(kHardwareConfigPath));
	if (hwConfig && values.invert != initial.invert) {
		if (hqcore::HardwareConfig::saveMotorInverts(kHardwareConfigPath, values.invert[0],
							     values.invert[1], values.invert[2])) {
			for (int i = 0; i < 3; ++i) {
				if (motorRows_[i].motor)
					motorRows_[i].motor->setInvert(values.invert[i]);
			}
		} else {
			failures << QString("Could not write the motor directions to %1.")
					    .arg(QString::fromStdString(kHardwareConfigPath));
		}
	}

	applyExportSettings();

	if (!failures.isEmpty()) {
		QMessageBox::warning(this, "Settings not fully saved", failures.join("\n"));
		statusBar()->showMessage("Some settings could not be saved.", 5000);
		return;
	}
	statusBar()->showMessage(
		values.exportSettings.mode == hqcore::ExportSettings::Mode::Local
			? QString("Settings saved - exporting to %1/%2.")
				  .arg(QString::fromStdString(values.exportSettings.localPath),
				       projectEdit_->text())
			: QString("Settings saved - exporting via FTP."),
		5000);
}

bool CaptureWindow::applyFilmFormat(const QString &name) {
	filmFormat_ =
		hqcore::HardwareConfig::loadFilmFormat(kHardwareConfigPath, name.toStdString());
	if (!filmFormat_) {
		sequenceButton_->setEnabled(false);
		sequenceButton_->setToolTip(
			QString("Sequence unavailable: %1 has no valid \"%2\" film format.")
				.arg(QString::fromStdString(kHardwareConfigPath), name));
		return false;
	}

	sequenceButton_->setEnabled(true);
	sequenceButton_->setToolTip(QString());

	// Show the configured starting speed - runSequenceLoop's
	// motorSpeedChanged emissions take over once a sequence actually
	// runs and adaptation kicks in.
	if (motorRows_[MotorFeeder].speedLabel) {
		motorRows_[MotorFeeder].speedLabel->setText(
			QString("%1 sps").arg(filmFormat_->feeder.speed, 0, 'f', 0));
	}
	if (motorRows_[MotorPickup].speedLabel) {
		motorRows_[MotorPickup].speedLabel->setText(
			QString("%1 sps").arg(filmFormat_->pickup.speed, 0, 'f', 0));
	}
	return true;
}

void CaptureWindow::onFilmFormatChanged(int index) {
	Q_UNUSED(index);
	if (filmFormatCombo_)
		applyFilmFormat(filmFormatCombo_->currentText());
}

void CaptureWindow::keyPressEvent(QKeyEvent *event) {
	if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
	    captureFrameButton_->isEnabled()) {
		captureFrameButton_->click();
		return;
	}

	if ((event->modifiers() & Qt::KeypadModifier) && !event->isAutoRepeat() &&
	    isMotorControlButtonFocused()) {
		if (auto mapping = motorKeyMappingFor(event->key())) {
			MotorRow &row = motorRows_[mapping->motorIndex];
			if (row.motor) {
				switch (mapping->action) {
				case MotorKeyAction::Ccw:
					row.motor->startJog(hqcore::MotorDirection::Ccw);
					numpadJogActive_[mapping->motorIndex] = true;
					break;
				case MotorKeyAction::Cw:
					row.motor->startJog(hqcore::MotorDirection::Cw);
					numpadJogActive_[mapping->motorIndex] = true;
					break;
				case MotorKeyAction::Toggle:
					onMotorPowerClicked(mapping->motorIndex);
					break;
				}
			}
			return;
		}
	}

	QMainWindow::keyPressEvent(event);
}

void CaptureWindow::keyReleaseEvent(QKeyEvent *event) {
	// Deliberately not gated on isMotorControlButtonFocused() here - see
	// the override's declaration in the header for why: a jog must stop
	// on release even if focus moved away while the key was held.
	if (!event->isAutoRepeat()) {
		if (auto mapping = motorKeyMappingFor(event->key());
		    mapping && mapping->action != MotorKeyAction::Toggle &&
		    numpadJogActive_[mapping->motorIndex]) {
			numpadJogActive_[mapping->motorIndex] = false;
			if (motorRows_[mapping->motorIndex].motor)
				motorRows_[mapping->motorIndex].motor->stopJog();
			return;
		}
	}

	QMainWindow::keyReleaseEvent(event);
}

bool CaptureWindow::isMotorControlButtonFocused() const {
	QWidget *focused = QApplication::focusWidget();
	if (!focused)
		return false;
	for (const auto &row : motorRows_) {
		if (focused == row.ccwButton || focused == row.powerButton ||
		    focused == row.cwButton) {
			return true;
		}
	}
	return false;
}

void CaptureWindow::setUpMotors(QWidget *central, QVBoxLayout *layout) {
	auto hwConfig = hqcore::HardwareConfig::load(kHardwareConfigPath);
	if (!hwConfig) {
		layout->addWidget(new QLabel(
			QString("Motor controls unavailable (%1 missing or invalid).")
				.arg(QString::fromStdString(kHardwareConfigPath)),
			central));
		return;
	}

	// filmdrive stops instantly on release (desired feel for the
	// sprocket-driven transport); feeder/pickup decelerate instead
	// (reel inertia/tension makes an instant stop harsh) - see
	// core/Motor.h. feeder/pickup also start already enabled (never
	// disabled even momentarily during startup) so launching the app
	// never drops the film tension they're already holding; filmdrive
	// starts disabled, which is fine since it isn't holding any tension.
	struct MotorSpec {
		const char *label;
		const hqcore::MotorConfig &config;
		bool decelerateOnStop;
		bool startEnabled;
	};
	const MotorSpec specs[3] = {
		{"Feeder", hwConfig->feeder, true, true},
		{"Filmdrive", hwConfig->filmdrive, false, false},
		{"Pickup", hwConfig->pickup, true, true},
	};
	const QIcon ccwIcon(kCcwIconPath);
	const QIcon cwIcon(kCwIconPath);
	const QIcon powerOnIcon(kPowerOnIconPath);
	const QIcon powerOffIcon(kPowerOffIconPath);

	// All three motors on one row, in the machine's physical order
	// (feeder left, filmdrive middle, pickup right).
	auto *motorsRow = new QHBoxLayout();
	motorsRow->setSpacing(16);
	layout->addLayout(motorsRow);

	for (int i = 0; i < 3; ++i) {
		MotorRow &row = motorRows_[i];
		row.motor = hqcore::Motor::open(specs[i].config, specs[i].decelerateOnStop,
						     specs[i].startEnabled);

		auto *rowLayout = new QHBoxLayout();

		row.sensorIndicator = new QLabel(central);
		row.sensorIndicator->setFixedSize(14, 14);
		row.sensorIndicator->setStyleSheet(kSensorIndicatorOffStyle);
		rowLayout->addWidget(row.sensorIndicator);

		auto *nameLabel = new QLabel(specs[i].label, central);
		rowLayout->addWidget(nameLabel);

		row.ccwButton = new QPushButton(central);
		row.ccwButton->setIcon(ccwIcon);
		rowLayout->addWidget(row.ccwButton);

		row.powerButton = new QPushButton(central);
		row.powerButton->setIcon(row.motor && row.motor->isEnabled() ? powerOnIcon
									      : powerOffIcon);
		rowLayout->addWidget(row.powerButton);

		row.cwButton = new QPushButton(central);
		row.cwButton->setIcon(cwIcon);
		rowLayout->addWidget(row.cwButton);

		row.speedLabel = new QLabel(central);
		row.speedLabel->setMinimumWidth(62);
		row.speedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		// Only feeder/pickup have an adaptive speed (SpeedAdapter) -
		// filmdrive runs advanceFixedSteps/advanceUntilSensor at a
		// fixed speed, so its label just says so instead of showing a
		// number that would never change and could look broken.
		row.speedLabel->setText(i == MotorFilmdrive ? "fixed" : "--");
		rowLayout->addWidget(row.speedLabel);
		rowLayout->addStretch(1);

		motorsRow->addLayout(rowLayout, 1);

		if (!row.motor) {
			row.ccwButton->setEnabled(false);
			row.powerButton->setEnabled(false);
			row.cwButton->setEnabled(false);
			continue;
		}

		// Press-and-hold jog, no interlock on enable state - matches
		// the real hardware: step pulses while disabled simply have
		// no effect, same as ../GugusseRoller's own wiring.
		connect(row.ccwButton, &QPushButton::pressed, this, [this, i] {
			if (motorRows_[i].motor)
				motorRows_[i].motor->startJog(hqcore::MotorDirection::Ccw);
		});
		connect(row.ccwButton, &QPushButton::released, this, [this, i] {
			if (motorRows_[i].motor)
				motorRows_[i].motor->stopJog();
		});
		connect(row.cwButton, &QPushButton::pressed, this, [this, i] {
			if (motorRows_[i].motor)
				motorRows_[i].motor->startJog(hqcore::MotorDirection::Cw);
		});
		connect(row.cwButton, &QPushButton::released, this, [this, i] {
			if (motorRows_[i].motor)
				motorRows_[i].motor->stopJog();
		});
		connect(row.powerButton, &QPushButton::clicked, this,
			[this, i] { onMotorPowerClicked(i); });
	}
}

void CaptureWindow::onSensorPollTimer() {
	for (auto &row : motorRows_) {
		if (!row.motor || !row.sensorIndicator)
			continue;
		const bool triggered = row.motor->sensorTriggered();
		if (row.lastSensorState && *row.lastSensorState == triggered)
			continue;
		row.lastSensorState = triggered;
		row.sensorIndicator->setStyleSheet(triggered ? kSensorIndicatorOnStyle
							      : kSensorIndicatorOffStyle);
	}
}

void CaptureWindow::onMotorSpeedChanged(int motorIndex, double speed) {
	if (motorIndex < 0 || motorIndex >= static_cast<int>(motorRows_.size()))
		return;
	if (QLabel *label = motorRows_[motorIndex].speedLabel)
		label->setText(QString("%1 sps").arg(speed, 0, 'f', 0));
}

void CaptureWindow::reportFault(FaultType type, MotorIndex motor) {
	bool expected = false;
	if (faultClaimed_.compare_exchange_strong(expected, true)) {
		std::lock_guard<std::mutex> lock(faultMutex_);
		firstFault_ = FaultInfo{type, motor};
	}
}

void CaptureWindow::recordSensorTriggerForShortFault(MotorIndex motor, long steps,
							 long shortThresholdSteps) {
	if (steps < shortThresholdSteps) {
		if (++shortsInARow_[motor] >= 10)
			reportFault(FaultType::MotorShortFault, motor);
	} else {
		shortsInARow_[motor] = 0;
	}
}

QString CaptureWindow::formatFaultMessage(const FaultInfo &fault) const {
	switch (fault.type) {
	case FaultType::MotorLongFault:
		return QString("FAULT: %1 - long fault (sensor not detected within "
				"faultTreshold steps).")
			.arg(kMotorNames[fault.motor]);
	case FaultType::MotorShortFault:
		return QString("FAULT: %1 - short fault (10 consecutive triggers under "
				"the short-fault threshold).")
			.arg(kMotorNames[fault.motor]);
	case FaultType::UploadBacklogFault:
		return QString("FAULT: upload backlog - %1 or more files pending in %2 "
				"for over 10 minutes.")
			.arg(kMaxPendingUploads)
			.arg(kShmCompleteDir);
	}
	return "FAULT: unknown.";
}

int CaptureWindow::countPendingUploads() {
	namespace fs = std::filesystem;
	std::error_code ec;
	int count = 0;
	for (const auto &entry : fs::directory_iterator(kShmCompleteDir, ec)) {
		if (entry.is_regular_file())
			++count;
	}
	return count;
}

void CaptureWindow::runSequenceLoop() {
	using hqcore::MotionResult;

	hqcore::Motor &feeder = *motorRows_[MotorFeeder].motor;
	hqcore::Motor &filmdrive = *motorRows_[MotorFilmdrive].motor;
	hqcore::Motor &pickup = *motorRows_[MotorPickup].motor;
	const auto &fmt = *filmFormat_;
	bool problemStop = false; // true for any fault/error break (not gentle, not emergency)

	// Recalibrates feeder/pickup's peak "speed" cycle to cycle so the arm
	// sensor triggers right at targetTime - see core/SpeedAdapter.h. Local
	// to this run (not a CaptureWindow member): a fresh sequence starts
	// from the film format's configured speed every time, not wherever a
	// previous run's adaptation happened to land.
	hqcore::SpeedAdapter feederSpeedAdapter(fmt.feeder.speed, fmt.feeder.speed2,
						     feeder.config().minSpeed, feeder.config().maxSpeed,
						     fmt.feeder.targetTime);
	hqcore::SpeedAdapter pickupSpeedAdapter(fmt.pickup.speed, fmt.pickup.speed2,
						     pickup.config().minSpeed, pickup.config().maxSpeed,
						     fmt.pickup.targetTime);

	// White light stays on for the whole run (colour sensor - no per-channel
	// lighting); switched off again once the run ends, however it ends.
	if (light_)
		light_->set(LightController::Color::White);

	while (!gentleStopRequested_ && !emergencyStopRequested_) {
		// Backpressure: hold the next capture while /dev/shm/complete
		// has too many files still waiting for FTP to drain them -
		// otherwise a slow/unreachable server could let tmpfs usage
		// grow unbounded. If it never clears within 10 minutes
		// straight, that's a fault.
		{
			const auto waitStart = std::chrono::steady_clock::now();
			while (countPendingUploads() >= kMaxPendingUploads) {
				if (emergencyStopRequested_ || gentleStopRequested_)
					break;
				const double waited = std::chrono::duration<double>(
							       std::chrono::steady_clock::now() -
							       waitStart)
							       .count();
				if (waited > kUploadBacklogFaultSeconds) {
					reportFault(FaultType::UploadBacklogFault);
					break;
				}
				emit sequenceStatusChanged(
					QString("Holding: %1 files waiting for upload "
						"(limit %2)...")
						.arg(countPendingUploads())
						.arg(kMaxPendingUploads));
				std::this_thread::sleep_for(
					std::chrono::milliseconds(kUploadBacklogPollMs));
			}
		}
		if (emergencyStopRequested_) {
			emit sequenceStatusChanged("Emergency stop.");
			break;
		}
		if (faultClaimed_) {
			problemStop = true;
			emit sequenceStatusChanged(formatFaultMessage(*firstFault_));
			break;
		}
		if (gentleStopRequested_)
			break; // clean stop while holding for the upload backlog

		emit sequenceStatusChanged("A: capturing frame...");
		QString capturedName;
		bool captureOk = captureAndStage(&emergencyStopRequested_, capturedName);
		if (emergencyStopRequested_) {
			emit sequenceStatusChanged("Emergency stop.");
			break;
		}
		if (!captureOk) {
			emit sequenceStatusChanged("Capture/save failed - sequence stopped.");
			problemStop = true;
			break;
		}

		emit sequenceStatusChanged("B: feeder/pickup advancing to arm...");
		// Reflects the speed each motor is ABOUT to use this cycle in
		// its row's speed label (see MotorRow::speedLabel).
		emit motorSpeedChanged(MotorFeeder, feederSpeedAdapter.currentSpeed());
		emit motorSpeedChanged(MotorPickup, pickupSpeedAdapter.currentSpeed());
		hqcore::MotionOutcome feederOutcome;
		hqcore::MotionOutcome pickupOutcome;
		std::thread feederThread([&] {
			feederOutcome = feeder.moveTriangleUntilSensor(
				hqcore::MotorDirection::Ccw, feederSpeedAdapter.currentSpeed(),
				feederSpeedAdapter.currentSpeed2(), fmt.feeder.targetTime,
				fmt.feeder.faultTreshold, emergencyStopRequested_);
		});
		std::thread pickupThread([&] {
			pickupOutcome = pickup.moveTriangleUntilSensor(
				hqcore::MotorDirection::Ccw, pickupSpeedAdapter.currentSpeed(),
				pickupSpeedAdapter.currentSpeed2(), fmt.pickup.targetTime,
				fmt.pickup.faultTreshold, emergencyStopRequested_);
		});
		feederThread.join(); // C: join both threads
		pickupThread.join();

		if (emergencyStopRequested_) {
			emit sequenceStatusChanged("Emergency stop.");
			break;
		}

		// Long faults are immediate (per-call); short faults are a
		// rolling count across iterations - only fed by genuinely
		// successful triggers, not long-faults/aborts. Same for speed
		// recalibration: only a genuine sensor trigger is a
		// trustworthy timing sample.
		if (feederOutcome.result == MotionResult::LongFault)
			reportFault(FaultType::MotorLongFault, MotorFeeder);
		if (pickupOutcome.result == MotionResult::LongFault)
			reportFault(FaultType::MotorLongFault, MotorPickup);
		if (feederOutcome.result == MotionResult::Success) {
			recordSensorTriggerForShortFault(MotorFeeder, feederOutcome.steps,
							  fmt.feeder.ignoreInitial);
			feederSpeedAdapter.recordCycleAndGetNextSpeed(feederOutcome.elapsedSeconds);
		}
		if (pickupOutcome.result == MotionResult::Success) {
			recordSensorTriggerForShortFault(MotorPickup, pickupOutcome.steps,
							  fmt.pickup.ignoreInitial);
			pickupSpeedAdapter.recordCycleAndGetNextSpeed(pickupOutcome.elapsedSeconds);
		}
		if (faultClaimed_) {
			problemStop = true;
			emit sequenceStatusChanged(formatFaultMessage(*firstFault_));
			break;
		}

		emit sequenceStatusChanged("D: filmdrive advancing fixed steps...");
		bool filmdriveOk = filmdrive.advanceFixedSteps(
			hqcore::MotorDirection::Cw, fmt.filmdrive.ignoreInitial,
			fmt.filmdrive.speed, fmt.filmdrive.speed2, emergencyStopRequested_);
		if (emergencyStopRequested_) {
			emit sequenceStatusChanged("Emergency stop.");
			break;
		}
		if (!filmdriveOk) {
			emit sequenceStatusChanged("Filmdrive advance aborted.");
			problemStop = true;
			break;
		}

		emit sequenceStatusChanged("E: filmdrive advancing to hole...");
		auto holeOutcome = filmdrive.advanceUntilSensor(
			hqcore::MotorDirection::Cw, fmt.filmdrive.speed2,
			fmt.filmdrive.faultTreshold, emergencyStopRequested_);
		if (emergencyStopRequested_) {
			emit sequenceStatusChanged("Emergency stop.");
			break;
		}
		if (holeOutcome.result == MotionResult::LongFault) {
			reportFault(FaultType::MotorLongFault, MotorFilmdrive);
		} else if (holeOutcome.result == MotionResult::Success) {
			// See kFilmdriveShortFaultSteps: D already consumes
			// exactly ignoreInitial steps, so E's own count is
			// compared against a much smaller cutoff instead.
			recordSensorTriggerForShortFault(MotorFilmdrive, holeOutcome.steps,
							  kFilmdriveShortFaultSteps);
		}
		if (faultClaimed_) {
			problemStop = true;
			emit sequenceStatusChanged(formatFaultMessage(*firstFault_));
			break;
		}

		emit sequenceStatusChanged("F: settling...");
		std::this_thread::sleep_for(std::chrono::milliseconds(kSequenceSettleDelayMs));
		// G: loop back to A (checked at top of the while condition)
	}

	if (light_)
		light_->set(LightController::Color::Off);

	if (problemStop) {
		// Unlike Emergency Stop (which deliberately leaves motors
		// enabled), a genuine fault powers everything down.
		for (auto &row : motorRows_) {
			if (row.motor)
				row.motor->disable();
		}
	} else if (!emergencyStopRequested_) {
		emit sequenceStatusChanged("Sequence stopped.");
	}

	emit sequenceFinished();
}

void CaptureWindow::onSequenceButtonClicked() {
	if (sequenceRunning_) {
		gentleStopRequested_ = true;
		sequenceButton_->setEnabled(false);
		statusLabel_->setText("Finishing current iteration, then stopping...");
		return;
	}

	if (indexLookupPending_) {
		statusLabel_->setText("Checking the project directory for existing frames - "
				      "try again in a moment.");
		return;
	}

	if (!filmFormat_ || !camera_ ||
	    !motorRows_[MotorFeeder].motor || !motorRows_[MotorFilmdrive].motor ||
	    !motorRows_[MotorPickup].motor) {
		QMessageBox::critical(this, "Cannot start",
				      "Camera, lights, motors, or film format settings "
				      "are unavailable.");
		return;
	}

	gentleStopRequested_ = false;
	emergencyStopRequested_ = false;
	faultClaimed_ = false;
	firstFault_ = std::nullopt;
	shortsInARow_ = {};

	for (auto &row : motorRows_) {
		row.motor->enable();
		row.powerButton->setIcon(QIcon(kPowerOnIconPath));
	}

	captureFrameButton_->setEnabled(false);
	lightButton_->setEnabled(false);
	saveSettingsButton_->setEnabled(false);
	projectEdit_->setEnabled(false);
	filmFormatCombo_->setEnabled(false);
	// Export mode/motor directions must not change under a running sequence.
	otherSettingsButton_->setEnabled(false);
	// Switching format restarts the camera - not while a run is in flight.
	// (The camera controls stay live: a frame is only ever kept once it
	// provably reflects the current settings, see HqCamera::captureStill.)
	captureFormatCombo_->setEnabled(false);
	for (auto &row : motorRows_) {
		row.ccwButton->setEnabled(false);
		row.powerButton->setEnabled(false);
		row.cwButton->setEnabled(false);
	}

	sequenceRunning_ = true;
	sequenceButton_->setText("Stop After This Frame");

	if (sequenceThread_.joinable())
		sequenceThread_.join();
	sequenceThread_ = std::thread(&CaptureWindow::runSequenceLoop, this);
}

void CaptureWindow::onEmergencyStopClicked() {
	emergencyStopRequested_ = true;
	gentleStopRequested_ = true;
	statusLabel_->setText("EMERGENCY STOP requested...");
}

void CaptureWindow::onSequenceStatusChanged(QString text) {
	statusLabel_->setText(text);
}

void CaptureWindow::onSequenceFinished() {
	sequenceRunning_ = false;
	sequenceButton_->setText("Start Sequence");

	lightButton_->setEnabled(light_.has_value());
	// The sequence switched the light off when it ended.
	lightButton_->blockSignals(true);
	lightButton_->setChecked(false);
	lightButton_->blockSignals(false);
	saveSettingsButton_->setEnabled(true);
	projectEdit_->setEnabled(true);
	filmFormatCombo_->setEnabled(filmFormatCombo_->count() > 0);
	otherSettingsButton_->setEnabled(true);
	captureFormatCombo_->setEnabled(true);
	for (auto &row : motorRows_) {
		if (!row.motor)
			continue;
		row.ccwButton->setEnabled(true);
		row.powerButton->setEnabled(true);
		row.cwButton->setEnabled(true);
		// A fault disables motors outright (unlike a normal or
		// emergency stop, both of which leave them enabled) - resync
		// the power icon to whatever actually happened.
		row.powerButton->setIcon(row.motor->isEnabled() ? QIcon(kPowerOnIconPath)
								 : QIcon(kPowerOffIconPath));
	}

	// Any fault (a motor's arm/hole sensor never triggered or triggered
	// suspiciously early many times running, or the FTP upload backlog
	// never draining) is something going wrong most likely to be noticed
	// unattended - at the end of a roll, or an unreachable server - so
	// leaving the camera streaming with nobody watching is pointless. Stop it and hold both capture buttons
	// disabled until the fault dialog below is actually dismissed,
	// instead of re-enabling immediately like a normal or emergency stop
	// does.
	const bool faulted = faultClaimed_ && firstFault_.has_value();

	if (faulted) {
		stopCameraForFault();
	} else {
		captureFrameButton_->setEnabled(true);
		sequenceButton_->setEnabled(filmFormat_.has_value());
	}

	if (faulted) {
		QMessageBox::critical(this, "Sequence fault", formatFaultMessage(*firstFault_));
		resumeCameraAfterFaultAcknowledged();
	}
}

void CaptureWindow::onMotorPowerClicked(int index) {
	MotorRow &row = motorRows_[index];
	if (!row.motor)
		return;

	if (row.motor->isEnabled()) {
		row.motor->disable();
		row.powerButton->setIcon(QIcon(kPowerOffIconPath));
	} else {
		row.motor->enable();
		row.powerButton->setIcon(QIcon(kPowerOnIconPath));
	}
}
