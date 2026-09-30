cmake_minimum_required(VERSION 3.25)

# Static CRT so the result is a single self-contained .exe (matches the
# x64-windows-static vcpkg triplet).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(CallRecorder VERSION 1.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Opus CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_library(OPUSENC_LIBRARY NAMES opusenc REQUIRED)

# Version numbers for the .rc file's VERSIONINFO.
configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")

add_executable(CallRecorder WIN32
    src/main.cpp
    src/Autostart.cpp
    src/Config.cpp
    src/HistoryWindow.cpp
    src/SettingsDialog.cpp
    src/HttpClient.cpp
    src/OpusFileWriter.cpp
    src/Recorder.cpp
    src/Session.cpp
    src/Uploader.cpp
    src/Util.cpp
    res/CallRecorder.rc
)
target_include_directories(CallRecorder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(CallRecorder PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    $<$<COMPILE_LANGUAGE:CXX>:CALLRECORDER_VERSION="${PROJECT_VERSION}">
    $<$<COMPILE_LANGUAGE:CXX>:CALLRECORDER_VERSION_W=L"${PROJECT_VERSION}">
)
target_compile_options(CallRecorder PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
# res/CallRecorder.manifest replaces the linker's default one.
target_link_options(CallRecorder PRIVATE /MANIFEST:NO)

target_link_libraries(CallRecorder PRIVATE
    ${OPUSENC_LIBRARY} Opus::opus nlohmann_json::nlohmann_json
    ole32 shell32 user32 gdi32 advapi32 winhttp comctl32 avrt mmdevapi
)
