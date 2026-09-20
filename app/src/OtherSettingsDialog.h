#pragma once

#include <QDialog>

#include <array>
#include <atomic>
#include <functional>
#include <thread>

#include "../core/ExportSettings.h"
#include "../core/FtpConfig.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLineEdit;
class QPushButton;

// Modal "Other settings" window - the in-app replacement for
// ../GugusseRoller's standalone MotorsAndFtpSetup.py. Lets the operator
// choose how finished frames leave the machine (FTP upload with a
// connection test, or a local-copy directory such as a USB drive) and
// flip each motor's direction ("invert") with a test move.
//
// Pure UI: it edits a copy of the settings (Values) and hands the result
// back through values() after exec() returns Accepted - reading and
// writing the config files, and applying the result, is the caller's job.
class OtherSettingsDialog : public QDialog {
	Q_OBJECT
public:
	struct Values {
		hqcore::ExportSettings exportSettings;
		hqcore::FtpConfig ftp;
		// Indexed feeder, filmdrive, pickup.
		std::array<bool, 3> invert{};
	};

	// Runs a short test move of motor `motor` (0..2) with direction
	// inversion `invert` - must return promptly (do the move on another
	// thread) and later call `done` from the GUI thread once the motor
	// has stopped.
	using MotorTestFn =
		std::function<void(int motor, bool invert, std::function<void()> done)>;

	// `motorAvailable[i]` false disables motor i's controls (motor not
	// configured / GPIO unavailable).
	OtherSettingsDialog(const Values &initial, std::array<bool, 3> motorAvailable,
			    MotorTestFn testMotor, QWidget *parent = nullptr);
	~OtherSettingsDialog() override;

	Values values() const;

	// Refuses to close while a test is still running (the worker would
	// otherwise call back into a destroyed dialog).
	void done(int result) override;
	void accept() override;

private:
	void onModeChanged();
	void onBrowseClicked();
	void onTestFtpClicked();
	void onTestMotorClicked(int motor);
	hqcore::FtpConfig currentFtp() const;
	// Disables the whole dialog while a (slow) test runs.
	void setBusy(bool busy);

	QComboBox *modeCombo_ = nullptr;
	QGroupBox *ftpBox_ = nullptr;
	QLineEdit *serverEdit_ = nullptr;
	QLineEdit *userEdit_ = nullptr;
	QLineEdit *passwdEdit_ = nullptr;
	QLineEdit *ftpPathEdit_ = nullptr;
	QGroupBox *localBox_ = nullptr;
	QLineEdit *localPathEdit_ = nullptr;
	std::array<QCheckBox *, 3> invertChecks_{};
	std::array<QPushButton *, 3> motorTestButtons_{};

	MotorTestFn testMotor_;
	std::thread ftpTestThread_;
	bool busy_ = false;
};
