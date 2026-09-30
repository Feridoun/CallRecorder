cmake_minimum_required(VERSION 3.25)

# Static CRT so the result is a single self-contained .exe (matches the
# x64-windows-static vcpkg triplet).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MeetingRecorder VERSION 1.3.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Opus CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_library(OPUSENC_LIBRARY NAMES opusenc REQUIRED)

# Version numbers for the .rc file's VERSIONINFO.
configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")

add_executable(MeetingRecorder WIN32
    src/main.cpp
    src/Autostart.cpp
    src/CallDetector.cpp
    src/Config.cpp
    src/HistoryWindow.cpp
    src/Migration.cpp
    src/SettingsDialog.cpp
    src/HttpClient.cpp
    src/OpusFileWriter.cpp
    src/Recorder.cpp
    src/Session.cpp
    src/Uploader.cpp
    src/Util.cpp
    res/MeetingRecorder.rc
)
target_include_directories(MeetingRecorder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(MeetingRecorder PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    $<$<COMPILE_LANGUAGE:CXX>:MEETINGRECORDER_VERSION="${PROJECT_VERSION}">
    $<$<COMPILE_LANGUAGE:CXX>:MEETINGRECORDER_VERSION_W=L"${PROJECT_VERSION}">
)
target_compile_options(MeetingRecorder PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
# res/MeetingRecorder.manifest replaces the linker's default one.
target_link_options(MeetingRecorder PRIVATE /MANIFEST:NO)

target_link_libraries(MeetingRecorder PRIVATE
    ${OPUSENC_LIBRARY} Opus::opus nlohmann_json::nlohmann_json
    ole32 shell32 user32 gdi32 advapi32 winhttp comctl32 avrt mmdevapi
)

# Unit tests: the non-UI sources compiled straight into a console program, run
# with `ctest` (build.ps1 does this). They never touch %APPDATA%, the registry
# or the network; see tests/.
option(MEETINGRECORDER_TESTS "Build the unit tests (MeetingRecorderTests)" ON)
if(MEETINGRECORDER_TESTS)
    enable_testing()
    add_executable(MeetingRecorderTests
        tests/TestMain.cpp
        tests/CallLogicTests.cpp
        tests/ConfigTests.cpp
        tests/HttpTests.cpp
        tests/JsonTests.cpp
        tests/SessionTests.cpp
        tests/UploadLogicTests.cpp
        tests/UtilTests.cpp
        src/Config.cpp
        src/HttpClient.cpp
        src/Session.cpp
        src/Util.cpp
    )
    target_include_directories(MeetingRecorderTests PRIVATE src tests)
    target_compile_definitions(MeetingRecorderTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        $<$<COMPILE_LANGUAGE:CXX>:MEETINGRECORDER_VERSION="${PROJECT_VERSION}">
        $<$<COMPILE_LANGUAGE:CXX>:MEETINGRECORDER_VERSION_W=L"${PROJECT_VERSION}">
    )
    target_compile_options(MeetingRecorderTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
    target_link_libraries(MeetingRecorderTests PRIVATE
        nlohmann_json::nlohmann_json
        ole32 shell32 user32 advapi32 winhttp
    )
    add_test(NAME MeetingRecorderTests COMMAND MeetingRecorderTests)
endif()
