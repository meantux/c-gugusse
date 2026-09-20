#include "OtherSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include "../core/FtpTest.h"

namespace {
constexpr int kModeFtp = 0;
constexpr int kModeLocal = 1;

// Same wording as ../GugusseRoller's MotorsAndFtpSetup.py: after "Test
// motor" the operator checks the physical direction against this and
// ticks "invert" if it is the wrong way round. (The test always
// commands "cw".)
struct MotorText {
	const char *invertLabel;
	const char *expected;
};
constexpr MotorText kMotorText[3] = {
	{"Feeder invert", "The feeder plateau should turn clockwise."},
	{"Main drive invert", "The skateboard wheel should turn counter-clockwise."},
	{"Pickup invert", "The pickup plateau should turn clockwise."},
};
} // namespace

OtherSettingsDialog::OtherSettingsDialog(const Values &initial,
					   std::array<bool, 3> motorAvailable,
					   MotorTestFn testMotor, QWidget *parent)
	: QDialog(parent), testMotor_(std::move(testMotor)) {
	setWindowTitle("Other settings");
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	// --- Export ---
	auto *exportRow = new QHBoxLayout();
	exportRow->addWidget(new QLabel("Export mode:", this));
	modeCombo_ = new QComboBox(this);
	modeCombo_->addItem("FTP upload");
	modeCombo_->addItem("Local copy");
	modeCombo_->setToolTip(
		"Where finished frames go: uploaded to an FTP server, or moved to a\n"
		"directory on this machine (e.g. a USB drive). Either way they end up\n"
		"in a sub-directory named after the project.");
	exportRow->addWidget(modeCombo_);
	exportRow->addStretch(1);
	layout->addLayout(exportRow);

	ftpBox_ = new QGroupBox("FTP", this);
	auto *ftpForm = new QFormLayout(ftpBox_);
	serverEdit_ = new QLineEdit(QString::fromStdString(initial.ftp.server), ftpBox_);
	userEdit_ = new QLineEdit(QString::fromStdString(initial.ftp.user), ftpBox_);
	passwdEdit_ = new QLineEdit(QString::fromStdString(initial.ftp.passwd), ftpBox_);
	passwdEdit_->setEchoMode(QLineEdit::Password);
	auto *showPasswd = new QCheckBox("Show", ftpBox_);
	auto *passwdRow = new QHBoxLayout();
	passwdRow->addWidget(passwdEdit_, 1);
	passwdRow->addWidget(showPasswd);
	ftpPathEdit_ = new QLineEdit(QString::fromStdString(initial.ftp.path), ftpBox_);
	ftpPathEdit_->setToolTip("Directory on the server that the project directories are created in.");
	auto *testFtpButton = new QPushButton("Test FTP settings", ftpBox_);
	testFtpButton->setToolTip("Connects, creates a scratch directory, uploads a small file to\n"
				   "it and deletes both again.");
	ftpForm->addRow("Server address:", serverEdit_);
	ftpForm->addRow("Username:", userEdit_);
	ftpForm->addRow("Password:", passwdRow);
	ftpForm->addRow("Path:", ftpPathEdit_);
	ftpForm->addRow(testFtpButton);
	layout->addWidget(ftpBox_);

	localBox_ = new QGroupBox("Local copy", this);
	auto *localLayout = new QVBoxLayout(localBox_);
	auto *localRow = new QHBoxLayout();
	localPathEdit_ = new QLineEdit(QString::fromStdString(initial.exportSettings.localPath),
				       localBox_);
	auto *browseButton = new QPushButton("Browse...", localBox_);
	localRow->addWidget(new QLabel("Save path:", localBox_));
	localRow->addWidget(localPathEdit_, 1);
	localRow->addWidget(browseButton);
	localLayout->addLayout(localRow);
	auto *localHint = new QLabel("Frames are moved to <save path>/<project name>/. The save "
				     "path itself must already exist (mounted).",
				     localBox_);
	localHint->setWordWrap(true);
	localLayout->addWidget(localHint);
	layout->addWidget(localBox_);

	// --- Motors ---
	auto *motorsBox = new QGroupBox("Motor direction", this);
	auto *motorsLayout = new QVBoxLayout(motorsBox);
	for (int i = 0; i < 3; ++i) {
		auto *row = new QHBoxLayout();
		motorTestButtons_[i] = new QPushButton("Test motor", motorsBox);
		invertChecks_[i] = new QCheckBox(kMotorText[i].invertLabel, motorsBox);
		invertChecks_[i]->setChecked(initial.invert[i]);
		invertChecks_[i]->setToolTip(kMotorText[i].expected);
		motorTestButtons_[i]->setToolTip(
			QString("Turns the motor a short way \"clockwise\". %1\nIf it turned the "
				"wrong way, tick the invert box.")
				.arg(kMotorText[i].expected));
		row->addWidget(motorTestButtons_[i]);
		row->addWidget(invertChecks_[i]);
		row->addStretch(1);
		motorsLayout->addLayout(row);
		if (!motorAvailable[i]) {
			motorTestButtons_[i]->setEnabled(false);
			invertChecks_[i]->setEnabled(false);
			motorTestButtons_[i]->setToolTip("Motor unavailable (see hardwarecfg.json).");
		}
		connect(motorTestButtons_[i], &QPushButton::clicked, this,
			[this, i] { onTestMotorClicked(i); });
	}
	layout->addWidget(motorsBox);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);

	modeCombo_->setCurrentIndex(initial.exportSettings.mode ==
						     hqcore::ExportSettings::Mode::Local
					     ? kModeLocal
					     : kModeFtp);
	onModeChanged();

	connect(modeCombo_, &QComboBox::currentIndexChanged, this,
		&OtherSettingsDialog::onModeChanged);
	connect(showPasswd, &QCheckBox::toggled, this, [this](bool show) {
		passwdEdit_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
	});
	connect(browseButton, &QPushButton::clicked, this, &OtherSettingsDialog::onBrowseClicked);
	connect(testFtpButton, &QPushButton::clicked, this, &OtherSettingsDialog::onTestFtpClicked);
	connect(buttons, &QDialogButtonBox::accepted, this, &OtherSettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &OtherSettingsDialog::reject);
}

OtherSettingsDialog::~OtherSettingsDialog() {
	if (ftpTestThread_.joinable())
		ftpTestThread_.join();
}

void OtherSettingsDialog::onModeChanged() {
	const bool ftp = modeCombo_->currentIndex() == kModeFtp;
	ftpBox_->setEnabled(ftp);
	localBox_->setEnabled(!ftp);
}

void OtherSettingsDialog::onBrowseClicked() {
	QString start = localPathEdit_->text();
	if (!QFileInfo(start).isDir())
		start = "/media";
	const QString chosen = QFileDialog::getExistingDirectory(
		this, "Choose the save directory", start, QFileDialog::ShowDirsOnly);
	if (!chosen.isEmpty()) // empty = cancelled
		localPathEdit_->setText(chosen);
}

hqcore::FtpConfig OtherSettingsDialog::currentFtp() const {
	hqcore::FtpConfig cfg;
	cfg.server = serverEdit_->text().trimmed().toStdString();
	cfg.user = userEdit_->text().toStdString();
	cfg.passwd = passwdEdit_->text().toStdString();
	cfg.path = ftpPathEdit_->text().toStdString();
	return cfg;
}

OtherSettingsDialog::Values OtherSettingsDialog::values() const {
	Values v;
	v.exportSettings.mode = modeCombo_->currentIndex() == kModeLocal
					? hqcore::ExportSettings::Mode::Local
					: hqcore::ExportSettings::Mode::Ftp;
	v.exportSettings.localPath = localPathEdit_->text().trimmed().toStdString();
	v.ftp = currentFtp();
	for (int i = 0; i < 3; ++i)
		v.invert[i] = invertChecks_[i]->isChecked();
	return v;
}

void OtherSettingsDialog::setBusy(bool busy) {
	busy_ = busy;
	setEnabled(!busy);
}

void OtherSettingsDialog::done(int result) {
	if (busy_)
		return;
	QDialog::done(result);
}

void OtherSettingsDialog::accept() {
	if (busy_)
		return;
	const Values v = values();

	// Only the chosen export mode has to be usable - the other one's
	// fields are kept as typed either way.
	if (v.exportSettings.mode == hqcore::ExportSettings::Mode::Ftp) {
		if (v.ftp.server.empty()) {
			QMessageBox::warning(this, "FTP server missing",
					     "Enter the FTP server address, or switch the export "
					     "mode to Local copy.");
			return;
		}
	} else {
		if (v.exportSettings.localPath.empty()) {
			QMessageBox::warning(this, "Save path missing",
					     "Choose the directory to copy the frames to.");
			return;
		}
		if (!QFileInfo(QString::fromStdString(v.exportSettings.localPath)).isDir()) {
			const auto choice = QMessageBox::question(
				this, "Save path not found",
				QString("\"%1\" doesn't exist right now (drive not mounted?). "
					"Frames can't be copied until it does.\n\nSave anyway?")
					.arg(QString::fromStdString(v.exportSettings.localPath)),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (choice != QMessageBox::Yes)
				return;
		}
	}
	QDialog::accept();
}

void OtherSettingsDialog::onTestFtpClicked() {
	if (busy_)
		return;
	const hqcore::FtpConfig cfg = currentFtp();
	if (cfg.server.empty()) {
		QMessageBox::warning(this, "FTP server missing", "Enter the FTP server address first.");
		return;
	}

	if (ftpTestThread_.joinable())
		ftpTestThread_.join();
	setBusy(true);
	ftpTestThread_ = std::thread([this, cfg] {
		const QString error = QString::fromStdString(hqcore::testFtpConnection(cfg));
		QMetaObject::invokeMethod(
			this,
			[this, error] {
				setBusy(false);
				if (error.isEmpty())
					QMessageBox::information(this, "SUCCESS", "It seems to work.");
				else
					QMessageBox::critical(this, "FTP test failed", error);
			},
			Qt::QueuedConnection);
	});
}

void OtherSettingsDialog::onTestMotorClicked(int motor) {
	if (busy_ || !testMotor_)
		return;
	setBusy(true);
	testMotor_(motor, invertChecks_[motor]->isChecked(), [this, motor] {
		setBusy(false);
		QMessageBox::information(this, "Motor test", kMotorText[motor].expected);
	});
}
