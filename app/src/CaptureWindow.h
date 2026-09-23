#pragma once

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QVector>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "../core/CameraSettings.h"
#include "../core/FtpConfig.h"
#include "../core/FtpUploader.h"
#include "../core/HardwareConfig.h"
#include "../core/HqCamera.h"
#include "../core/LocalMover.h"
#include "../core/LightController.h"
#include "../core/Motor.h"

class QSlider;
class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QTimer;
class QVBoxLayout;
class HistogramWidget;
class ClickableLabel;

// Capture window for the Raspberry Pi HQ camera (a colour sensor - always
// lit with the white LEDs): live preview, a histogram of the
// sensor's true 12-bit raw values, a single exposure setting, red/blue
// white-balance gains, brightness/contrast/saturation, and a DNG-or-JPG
// choice for saved frames. Also drives the film transport (motors +
// sensors) and exports finished frames - FTP upload or local copy, see
// applyExportSettings().
//
// Layout convention: camera-related settings (light, format, camera
// controls) and the project row (project name, film format, "Other
// settings...") live in the top section, above the live preview;
// everything else (motor controls, sequence controls) lives in the bottom
// section, below it.
class CaptureWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit CaptureWindow(QWidget *parent = nullptr);
	~CaptureWindow() override;

signals:
	// Emitted from the camera thread's preview callback. The window lives
	// on the GUI thread, so Qt delivers this via a queued connection.
	// `histogram` is empty unless the histogram is enabled (then 4096 bins
	// of the raw 12-bit values). `zoomed` marks the 1:1 raw crop view.
	void frameReady(QImage image, bool zoomed, QVector<quint32> histogram);
	// Emitted from the background sequence thread (runSequenceLoop) -
	// queued connection marshals the status text update / re-enabling
	// of controls back onto the GUI thread.
	void sequenceStatusChanged(QString text);
	void sequenceFinished();
	// Emitted from runSequenceLoop right before each step-B move, so the
	// GUI's per-row speed label (see MotorRow::speedLabel) always shows
	// the speed about to be used - motorIndex is a MotorIndex value.
	void motorSpeedChanged(int motorIndex, double speed);
	// Emitted from the manual-capture worker thread when it is done.
	void manualCaptureFinished(bool ok, QString message);

protected:
	// Also handles the numpad motor jog/toggle shortcuts (7/8/9,
	// 4/5/6, 1/2/3 - feeder/filmdrive/pickup) while focus is on one of
	// the 9 manual motor buttons - see motorKeyMappingFor() in the .cpp
	// and isMotorControlButtonFocused().
	void keyPressEvent(QKeyEvent *event) override;
	// Stops a numpad-started jog on release - deliberately NOT gated on
	// isMotorControlButtonFocused() (unlike keyPressEvent): if focus
	// moved away while a jog key was still held, the release must still
	// stop it, or the motor would jog forever. See numpadJogActive_.
	void keyReleaseEvent(QKeyEvent *event) override;
	// Warns (Save/Discard/Cancel) if camera or film-format settings
	// have changed since the last "Save Settings" click - see
	// hasUnsavedSettings().
	void closeEvent(QCloseEvent *event) override;

private slots:
	void onFrameReady(QImage image, bool zoomed, QVector<quint32> histogram);
	void onHistogramToggled(bool enabled);
	void onLightToggled(bool on);
	void onHFlipToggled(bool enabled);
	void onVFlipToggled(bool enabled);
	void onInvertToggled(bool enabled);
	void onControlChanged();
	void onCaptureFormatChanged(int index);
	void onCaptureFrameClicked();
	void onManualCaptureFinished(bool ok, QString message);
	void onPreviewClicked(QPoint pos);
	void onSaveSettingsClicked();
	void onMotorPowerClicked(int index);
	void onProjectNameEdited();
	void onSequenceButtonClicked();
	// Checks and launches the sequence (the part of a Start press that
	// follows the start-index check).
	void startSequence();
	void onEmergencyStopClicked();
	void onSequenceStatusChanged(QString text);
	void onSequenceFinished();
	void onMotorSpeedChanged(int motorIndex, double speed);
	// Polls all 3 motors' arm/hole sensors at kSensorPollMs and updates
	// their little indicator dots - see MotorRow::sensorIndicator.
	void onSensorPollTimer();
	void onFilmFormatChanged(int index);
	// Opens the modal "Other settings" window (OtherSettingsDialog):
	// export mode (FTP / local copy), FTP credentials, motor direction.
	void onOtherSettingsClicked();

private:
	std::optional<QString> setUpCamera();
	// Camera controls as currently shown by the sliders.
	hqcore::CameraControls currentControls() const;
	// Same, in the persisted form (what Save Settings writes).
	hqcore::CameraSettings currentSettings() const;
	hqcore::CaptureFormat selectedCaptureFormat() const;
	void updateControlLabels();

	// Grabs one frame in the camera's current stream format (blocking - see
	// HqCamera::captureStill(), which guarantees the frame was exposed
	// after this call) and writes it to /dev/shm/<NNNNN>.<ext>, then stages
	// it into /dev/shm/complete/ (see frameCount_). No widget access - safe
	// to call from any thread. Returns the staged file name in `fileName`
	// on success. `abort` (optional) is checked before the capture starts.
	bool captureAndStage(std::atomic<bool> *abort, QString &fileName);

	struct ControlSlider {
		QSlider *slider = nullptr;
		QLabel *label = nullptr;
		double scale = 1.0; // slider integer value / scale = control value
	};
	ControlSlider exposureControl_;
	ControlSlider redGainControl_;
	ControlSlider blueGainControl_;
	ControlSlider brightnessControl_;
	ControlSlider contrastControl_;
	ControlSlider saturationControl_;

	QPushButton *lightButton_ = nullptr;
	QComboBox *captureFormatCombo_ = nullptr;
	ClickableLabel *previewLabel_ = nullptr;
	QLabel *statusLabel_ = nullptr;

	// Click-to-zoom-1:1 focus check: clicking inside the preview shows an
	// unprocessed 1:1 crop of the raw sensor pixels centred there (see
	// PreviewFrame::zoomed) - the downscaled preview can't show real
	// sharpness. Clicking again returns to the normal preview. previewScale_
	// converts a click on the displayed preview to sensor coordinates.
	bool zoomed_ = false;
	double previewScale_ = 4.0;

	QPushButton *captureFrameButton_ = nullptr;
	QPushButton *histogramToggleButton_ = nullptr;
	HistogramWidget *histogramWidget_ = nullptr;
	QTimer *histogramAutoOffTimer_ = nullptr;

	// Preview-only orientation/colour toggles. hFlip_/vFlip_ are also
	// recorded in captured DNGs' Orientation tag (see captureAndStage())
	// so viewers/editors show the frame the same way the preview did - the
	// raw pixels themselves are never flipped (that would require
	// remapping the CFA pattern). invert_ is for scanning negative film:
	// it only affects what's drawn on screen, never a saved file.
	QPushButton *hFlipButton_ = nullptr;
	QPushButton *vFlipButton_ = nullptr;
	QPushButton *invertButton_ = nullptr;
	bool hFlip_ = false;
	bool vFlip_ = false;
	bool invertPreview_ = false;

	std::optional<hqcore::LightController> light_;
	std::unique_ptr<hqcore::HqCamera> camera_;

	// Persisted camera settings (core/CameraSettings.h). Save Settings also
	// persists the film format, capture format and reel direction to
	// preferences.json via core/Preferences.h.
	QPushButton *saveSettingsButton_ = nullptr;
	hqcore::CameraSettings lastSavedSettings_;
	std::string lastSavedFilmFormatName_;
	std::string lastSavedCaptureFormat_;
	std::string lastSavedReelDirection_;
	// True if any saved setting has changed since the last successful
	// Save Settings - drives closeEvent()'s exit warning.
	bool hasUnsavedSettings() const;
	// True if the light is on or any motor is enabled - drives
	// closeEvent()'s "turn off before exiting?" prompt.
	bool anyHardwareOn() const;
	// Turns the light off and disables every motor - used by closeEvent()
	// when the operator chooses to turn everything off before exiting.
	void turnOffAllHardware();

	// Runs a single manual capture off the GUI thread (the capture waits
	// for fresh camera frames and writing a 12MP file takes a while).
	std::thread manualCaptureThread_;
	std::atomic<bool> manualCaptureRunning_{false};

	// Manual motor jog controls (feeder/filmdrive/pickup): CCW (hold),
	// enable/disable toggle, CW (hold) - see core/Motor.h. Simple
	// press-and-hold jog with a 10s min->max ramp. Pin wiring and speed
	// limits come from hardwarecfg.json (core/HardwareConfig.h); if that
	// file is missing/invalid, motor controls are hidden rather than
	// guessing at wiring.
	struct MotorRow {
		std::optional<hqcore::Motor> motor;
		QPushButton *ccwButton = nullptr;
		QPushButton *powerButton = nullptr;
		QPushButton *cwButton = nullptr;
		// Small dot on the left of the row showing sensorTriggered()
		// live (see onSensorPollTimer()) - not updated on every GPIO
		// change, just polled at kSensorPollMs, which is plenty for a
		// human-visible indicator.
		QLabel *sensorIndicator = nullptr;
		std::optional<bool> lastSensorState; // avoids redundant restyling
		// Current adapted speed on the right of the row (feeder/pickup
		// only - see SpeedAdapter; filmdrive has no adaptive speed and
		// just shows a fixed placeholder).
		QLabel *speedLabel = nullptr;
	};
	std::array<MotorRow, 3> motorRows_;

	void setUpMotors(QWidget *central, QVBoxLayout *layout);
	QTimer *sensorPollTimer_ = nullptr;

	// True while focus is on any of the 9 manual motor buttons (any
	// row's ccw/power/cw) - gates whether keyPressEvent() treats a
	// numpad key as a motor shortcut at all.
	bool isMotorControlButtonFocused() const;
	// Indexed by MotorIndex: true while that motor's jog was started by
	// a numpad key and hasn't been released yet - lets keyReleaseEvent()
	// stop the right motor without re-checking focus (see its override
	// declaration above for why).
	std::array<bool, 3> numpadJogActive_{};

	// Starts (or restarts, after stopCameraForFault() or a capture-format
	// change) camera streaming in the given format with the live-preview
	// callback. Returns an error message on failure.
	std::optional<QString> startCameraStream(hqcore::CaptureFormat format);
	// Stops video streaming and blanks the preview with a fault message,
	// disabling both capture buttons - called from onSequenceFinished()
	// on any sequence fault so an unattended rig doesn't keep the camera
	// streaming with nobody watching.
	void stopCameraForFault();
	// Restarts streaming and re-enables the capture buttons - called
	// right after the fault dialog is dismissed, so the camera stays off
	// for the whole time the fault is unacknowledged.
	void resumeCameraAfterFaultAcknowledged();

	// Project name (defaults to the app's start date/time, YYYYMMDD-HHMM)
	// - used as the FTP-upload / local-copy subdirectory name. Edits take effect on
	// editingFinished (Enter or focus-out), matching
	// ../GugusseRoller's own ProjectNameWidget.
	QLineEdit *projectEdit_ = nullptr;

	// Each completed capture is written to /dev/shm/<NNNNN>.<dng|jpg> then
	// renamed into /dev/shm/complete/ once the write finishes (atomic on
	// the same tmpfs) - matching ../GugusseRoller's captureCycle()
	// (write + os.rename). frameCount_ is the next frame number; it is
	// seeded from the destination project directory (see
	// refreshStartIndex()) - 0 if that is new or empty.
	std::atomic<int> frameCount_{0};
	// Serialises captureAndStage() between the manual-capture worker and
	// the sequence thread (frameCount_ numbering + camera still request).
	std::mutex captureMutex_;

	// Background FTP uploader (core/FtpUploader.h) watching
	// /dev/shm/complete and pushing files to ftp.json's server under
	// projectEdit_'s current text. std::nullopt if ftp.json is
	// missing/invalid - capture and local /dev/shm staging still work,
	// just nothing uploads.
	std::optional<hqcore::FtpUploader> ftpUploader_;
	// The local-copy alternative (core/LocalMover.h): moves the same
	// files into <localFilePath>/<project> instead. Exactly one of
	// ftpUploader_/localMover_ is alive at a time, chosen by
	// hardwarecfg.json's "saveMode" (see applyExportSettings()); both
	// are empty if FTP mode is selected but ftp.json is unusable.
	std::optional<hqcore::LocalMover> localMover_;
	// (Re)creates ftpUploader_/localMover_ from the config files -
	// called at startup and after the "Other settings" window saves.
	// Destroying the old exporter first waits for a transfer in flight.
	void applyExportSettings();
	// Looks at the destination project directory (FTP or local, see
	// core/FrameIndex.h) and re-seeds frameCount_ with one past the
	// highest frame number already there - 0 for a new/empty project - so
	// a reused project name continues its numbering instead of
	// overwriting. Runs the (possibly slow) listing on a worker thread;
	// until it reports back, indexLookupPending_ holds captures off.
	// Called from applyExportSettings() and when the project name
	// changes.
	void refreshStartIndex();
	bool indexLookupPending_ = false;
	// Start Sequence pressed while the lookup was still running: the
	// sequence starts by itself when the lookup reports success (and is
	// dropped if it fails or the project/destination changes meanwhile).
	bool startWhenReady_ = false;
	// Seconds the pending lookup may take at most (0: no known bound, e.g.
	// a local directory) and when it began - drive the countdown.
	int indexLookupTimeoutSec_ = 0;
	std::chrono::steady_clock::time_point indexLookupStart_;
	// The last lookup failed: the "next frame" number is only a guess.
	bool indexLookupFailed_ = false;
	// Shows the frame number the next capture gets, or the lookup's
	// progress; refreshed by indexUiTimer_ (also keeps the Start button's
	// text and the countdown current).
	QLabel *nextFrameLabel_ = nullptr;
	QTimer *indexUiTimer_ = nullptr;
	void updateIndexUi();
	void cancelStartWhenReady(const QString &why);
	// Only the newest lookup's result is used (the project name can
	// change again while an older one is still listing).
	uint64_t indexLookupGeneration_ = 0;
	std::string lastIndexedProject_;
	// Runs OtherSettingsDialog's "Test motor" move off the GUI thread:
	// with direction-inversion `invert`, turns motor `motorIndex`
	// clockwise a short way, then queues `done` back onto the GUI thread.
	void runMotorTest(int motorIndex, bool invert, std::function<void()> done);
	std::thread motorTestThread_;
	QPushButton *otherSettingsButton_ = nullptr;

	// --- Automated capture-loop sequence ---
	// A - capture a frame (white light on for the whole run); B -
	// feeder+pickup simultaneously advance (triangular ramp) until their
	// arm sensors trigger, decelerating to 0 over 0.1s on trigger; C -
	// join; D - filmdrive advances a fixed step count (ramped, sensor
	// ignored); E - filmdrive advances at constant speed2 until the hole
	// sensor triggers (instant stop); F - 0.1s settle delay; G - repeat.
	// Uses the film format chosen in the dropdown (hardwarecfg.json's
	// filmFormats). Deliberately excludes ../GugusseRoller's dynamic speed
	// recalibration of the filmdrive.
	//
	// Runs entirely on sequenceThread_ (never the GUI thread - a single
	// iteration takes multiple seconds and involves blocking sensor
	// polling, which would otherwise freeze the UI and the Stop/
	// Emergency Stop buttons with it). Qt widget access from that thread
	// is not allowed, hence sequenceStatusChanged/sequenceFinished being
	// signals rather than direct calls.
	enum MotorIndex { MotorFeeder = 0, MotorFilmdrive = 1, MotorPickup = 2 };

	// The sequence thread's entry point - loops A-G until
	// gentleStopRequested_ (checked only between iterations) or
	// emergencyStopRequested_ (checked throughout) fires, or a step
	// faults (sensor never triggered / capture failed).
	void runSequenceLoop();

	std::optional<hqcore::FilmFormatConfig> filmFormat_;
	// On the project row (see the class comment) - lets the film format be changed without editing
	// hardwarecfg.json by hand. Populated from
	// HardwareConfig::listFilmFormatNames() at startup; the choice
	// itself is NOT hardware config, so it's persisted separately (see
	// core/Preferences.h), not written back to hardwarecfg.json.
	QComboBox *filmFormatCombo_ = nullptr;
	// (Re)loads filmFormat_ for `name` and updates sequenceButton_'s
	// enabled state/tooltip accordingly - shared by startup and
	// onFilmFormatChanged() so they can't drift apart. Returns whether
	// the format loaded successfully.
	bool applyFilmFormat(const QString &name);
	// Which way feeder and pickup both turn during a sequence (one
	// setting for both). Persisted to preferences.json with the film
	// format; read once at sequence start, and locked while one runs.
	QComboBox *reelDirectionCombo_ = nullptr;
	hqcore::MotorDirection selectedReelDirection() const;
	// selectedReelDirection() snapshotted by startSequence() on the GUI
	// thread, for runSequenceLoop() to use without touching the widget.
	hqcore::MotorDirection sequenceReelDirection_ = hqcore::MotorDirection::Ccw;
	QPushButton *sequenceButton_ = nullptr;
	QPushButton *emergencyStopButton_ = nullptr;
	// Closes the window (see closeEvent()) - disabled while a sequence is
	// running, matching sequenceButton_/motor/etc.'s own disabling in
	// startSequence()/onSequenceFinished(). Wired straight to
	// QWidget::close() rather than a dedicated slot; all the actual
	// exit logic lives in closeEvent(), so Alt+F4/the window's close box
	// get exactly the same checks.
	QPushButton *quitButton_ = nullptr;
	std::thread sequenceThread_;
	std::atomic<bool> sequenceRunning_{false};
	std::atomic<bool> gentleStopRequested_{false};
	std::atomic<bool> emergencyStopRequested_{false};

	// --- Fault detection ---
	// On any fault: all motors are disabled (unlike Emergency Stop,
	// which deliberately leaves them enabled) and the lights are turned
	// off, then the FIRST fault encountered is reported and latched -
	// later faults within the same run (e.g. feeder and pickup both
	// faulting around the same moment in step B, which run in parallel
	// threads) don't overwrite it. Three fault types:
	//  - MotorLongFault: a motor was sent more steps than
	//    faultTreshold while still waiting for its sensor to trigger.
	//  - MotorShortFault: 10 consecutive iterations in a row where a
	//    motor's sensor triggered without needing more than
	//    ignoreInitial steps (feeder/pickup: the whole B move; filmdrive:
	//    step E's OWN step count, since D always advances exactly
	//    ignoreInitial steps by construction, so comparing E's count
	//    against the full ignoreInitial would never fire - see
	//    kFilmdriveShortFaultSteps in the .cpp).
	//  - UploadBacklogFault: /dev/shm/complete held >=4 pending files
	//    for more than 10 minutes straight (see kMaxPendingUploads /
	//    kUploadBacklogFaultSeconds in the .cpp).
	enum class FaultType { MotorLongFault, MotorShortFault, UploadBacklogFault };
	struct FaultInfo {
		FaultType type;
		MotorIndex motor = MotorFeeder; // meaningful only for motor faults
	};

	// Claims `fault` as the run's fault if none has been claimed yet
	// (first-one-wins, safe to call from multiple threads at once - see
	// step B's parallel feeder/pickup threads). No-op if a fault was
	// already claimed this run.
	void reportFault(FaultType type, MotorIndex motor = MotorFeeder);
	// Updates shortsInARow_[motor] from one sensor-triggered move's
	// outcome and reports a MotorShortFault via reportFault() if it
	// reaches 10. Only call for a MotionResult::Success outcome (a
	// LongFault/Aborted result isn't a "short" trigger).
	void recordSensorTriggerForShortFault(MotorIndex motor, long steps,
					       long shortThresholdSteps);
	QString formatFaultMessage(const FaultInfo &fault) const;
	// Counts regular files directly inside /dev/shm/complete (used for
	// the upload-backlog hold/fault check).
	static int countPendingUploads();

	std::mutex faultMutex_;
	std::atomic<bool> faultClaimed_{false};
	std::optional<FaultInfo> firstFault_;
	std::array<int, 3> shortsInARow_{}; // indexed by MotorIndex
};
