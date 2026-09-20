#include <QApplication>
#include <QMessageBox>
#include <QString>

#include "../core/AppPaths.h"
#include "CaptureWindow.h"

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	// First run (or a deleted file): create any missing config file in
	// ~/.config/c-gugusse from the installed defaults.
	const auto seeded = hqcore::seedConfigFiles();
	if (!seeded.error.empty()) {
		QMessageBox::warning(nullptr, "c-gugusse",
				     QString("Could not create the default configuration files:\n%1")
					     .arg(QString::fromStdString(seeded.error)));
	}

	CaptureWindow window;
	// Fallback geometry if the window is ever taken out of fullscreen.
	window.resize(1200, 950);
	window.showFullScreen();

	return app.exec();
}
