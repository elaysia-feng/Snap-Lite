cmake_minimum_required(VERSION 3.20)
project(SnapLite VERSION 1.3.6 LANGUAGES CXX RC)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(SNAPLITE_ICON "${CMAKE_CURRENT_BINARY_DIR}/SnapLite.ico")
execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/normalize_ico.ps1"
            -InputIconPath "${CMAKE_CURRENT_SOURCE_DIR}/resources/SnapLite.ico"
            -OutputPath "${SNAPLITE_ICON}"
    RESULT_VARIABLE SNAP_LITE_ICON_NORMALIZE_RESULT
)
if(NOT SNAP_LITE_ICON_NORMALIZE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate Snap-Lite icon resource")
endif()

set(SNAPLITE_EDITOR_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/editor_toolbar_anime.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_anime_toolbar_source.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/editor_toolbar.cpp"
            "${SNAPLITE_EDITOR_SOURCE}"
    RESULT_VARIABLE SNAP_LITE_TOOLBAR_GENERATE_RESULT
)
if(NOT SNAP_LITE_TOOLBAR_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate compact anime toolbar source")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/SnapLite.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
    @ONLY
)

add_executable(SnapLite WIN32
    src/main.cpp
    src/app.cpp
    ${SNAPLITE_EDITOR_SOURCE}
    src/capture.cpp
    src/capture_settings.cpp
    src/pin_window.cpp
    src/snip_window.cpp
    src/snip_text_overlay.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
)

target_include_directories(SnapLite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(SnapLite PRIVATE cxx_std_17)
target_compile_definitions(
    SnapLite PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SNAP_LITE_VERSION=L"${PROJECT_VERSION}"
)
target_link_libraries(
    SnapLite PRIVATE
    user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi imm32
)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive- /utf-8)
    set_property(
        SOURCE src/snip_window.cpp
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/window_bounds_fix.h"
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/drag_repaint_fix.h"
    )
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)