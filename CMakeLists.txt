cmake_minimum_required(VERSION 3.20)
project(SnapLite VERSION 0.3.0 LANGUAGES CXX RC)

set(SNAP_LITE_GENERATED_ICON "${CMAKE_CURRENT_BINARY_DIR}/SnapLite.ico")
execute_process(
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/resources/generate_icon.ps1"
            -OutputPath "${SNAP_LITE_GENERATED_ICON}"
    RESULT_VARIABLE SNAP_LITE_ICON_RESULT
)
if(NOT SNAP_LITE_ICON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate Snap-Lite application icon")
endif()
set(SNAP_LITE_ICON_PATH "${SNAP_LITE_GENERATED_ICON}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/SnapLite.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc"
    @ONLY
)

add_executable(SnapLite WIN32
    src/main.cpp
    src/app.cpp
    src/capture.cpp
    src/editor_window.cpp
    src/pin_window.cpp
    src/snip_window.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc"
)

target_compile_features(SnapLite PRIVATE cxx_std_17)
target_include_directories(SnapLite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_compile_definitions(
    SnapLite PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SNAP_LITE_VERSION=L"${PROJECT_VERSION}"
)
target_link_libraries(SnapLite PRIVATE user32 gdi32 gdiplus shell32 ole32 uuid)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive- /utf-8)
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)
