#include <QApplication>

#include "CaptureWindow.h"

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	CaptureWindow window;
	// Fallback geometry if the window is ever taken out of fullscreen.
	window.resize(1200, 950);
	window.showFullScreen();

	return app.exec();
}
