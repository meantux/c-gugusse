cmake_minimum_required(VERSION 3.16)
project(c-gugusse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(TIFF REQUIRED)
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)

# Qt-free core: camera (libcamera), image writers (DNG/JPEG), GPIO (lights +
# motors), config files, FTP upload / local copy. Kept separate from the GUI so it stays
# reusable by headless tools.
add_library(hqcore STATIC
	core/HqCamera.cpp
	core/DngWriter.cpp
	core/JpegWriter.cpp
	core/Histogram.cpp
	core/Gpio.cpp
	core/LightController.cpp
	core/CameraSettings.cpp
	core/Preferences.cpp
	core/HardwareConfig.cpp
	core/Motor.cpp
	core/SpeedAdapter.cpp
	core/FtpConfig.cpp
	core/FtpUploader.cpp
	core/FtpTest.cpp
	core/LocalMover.cpp
	core/ExportSettings.cpp
	core/FrameIndex.cpp
)
target_include_directories(hqcore PUBLIC core)
target_link_libraries(hqcore PUBLIC
	Threads::Threads
	TIFF::TIFF
	JPEG::JPEG
	PkgConfig::GPIOD
	PkgConfig::LIBCAMERA
	nlohmann_json::nlohmann_json
	CURL::libcurl
)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# The capture application: live preview + raw histogram, single-exposure /
# white-balance / image controls, DNG or JPG saving, motor + film transport
# sequence, FTP upload.
add_executable(c-gugusse
	src/main.cpp
	src/CaptureWindow.cpp
	src/HistogramWidget.cpp
	src/ClickableLabel.cpp
	src/OtherSettingsDialog.cpp
)
target_link_libraries(c-gugusse PRIVATE hqcore Qt6::Widgets)
