cmake_minimum_required(VERSION 3.16)
project(Capture VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
)

set(PLATFORM_LIBS
    dwmapi.lib
    dxgi.lib
    d3d11.lib
    uuid.lib
    ole32.lib
    oleaut32.lib
    mfuuid.lib
    mf.lib
    mfplat.lib
    mfplay.lib
    mfreadwrite.lib
    shcore.lib
    user32.lib
    gdi32.lib
    winmm.lib
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -finput-charset=utf-8 -fexec-charset=utf-8")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

add_executable(Capture WIN32
    src/core/main.cpp
    src/core/CaptureController.cpp
    src/core/CaptureController.h
    src/core/RecordingController.cpp
    src/core/RecordingController.h
    src/capture/VideoCaptureDevice.cpp
    src/capture/VideoCaptureDevice.h
    src/capture/AudioCaptureDevice.cpp
    src/capture/AudioCaptureDevice.h
    src/encoding/EncoderFactory.cpp
    src/encoding/EncoderFactory.h
    src/encoding/Mp4Writer.cpp
    src/encoding/Mp4Writer.h
    src/ui/TrayIconManager.cpp
    src/ui/TrayIconManager.h
    src/config/HotkeyManager.cpp
    src/config/HotkeyManager.h
    src/ui/WindowPicker.cpp
    src/ui/WindowPicker.h
    src/config/SettingsManager.cpp
    src/config/SettingsManager.h
    src/ui/RegionSelector.cpp
    src/ui/RegionSelector.h
    src/ui/HotkeyCaptureLineEdit.cpp
    src/ui/HotkeyCaptureLineEdit.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    resources/app.rc
)

target_link_libraries(Capture
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    ${PLATFORM_LIBS}
)

if(WIN32)
    target_compile_definitions(Capture PRIVATE _UNICODE UNICODE _WIN32_WINNT=0x0A00)
endif()

set_target_properties(Capture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)

install(TARGETS Capture RUNTIME DESTINATION bin)
