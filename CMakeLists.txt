cmake_minimum_required(VERSION 3.20)
project(SnapLite VERSION 1.3.13 LANGUAGES CXX RC)

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

# Generate editor/capture sources in the build directory. This keeps the
# legacy capture engine stable while the eraser, larger undo history, and OCR
# integration are compiled into every local/CI/release build.
set(SNAPLITE_GENERATED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated_eraser_history")
set(SNAPLITE_GENERATED_SRC "${SNAPLITE_GENERATED_ROOT}/src")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_eraser_history_sources.py"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${SNAPLITE_GENERATED_ROOT}"
    RESULT_VARIABLE SNAP_LITE_ERASER_GENERATE_RESULT
)
if(NOT SNAP_LITE_ERASER_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate eraser/history sources")
endif()

set(SNAPLITE_EDITOR_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/editor_toolbar_anime.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_anime_toolbar_source.py"
            "${SNAPLITE_GENERATED_SRC}/editor_toolbar.cpp"
            "${SNAPLITE_EDITOR_SOURCE}"
    RESULT_VARIABLE SNAP_LITE_TOOLBAR_GENERATE_RESULT
)
if(NOT SNAP_LITE_TOOLBAR_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate compact anime toolbar source")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/add_toolbar_tooltips.py"
            "${SNAPLITE_EDITOR_SOURCE}"
    RESULT_VARIABLE SNAP_LITE_TOOLTIP_PATCH_RESULT
)
if(NOT SNAP_LITE_TOOLTIP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to add toolbar hover tooltips")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/add_ocr_action.py"
            "${SNAPLITE_EDITOR_SOURCE}"
            "${SNAPLITE_GENERATED_SRC}/toolbar_icon_render_gdi_anime.h"
    RESULT_VARIABLE SNAP_LITE_OCR_PATCH_RESULT
)
if(NOT SNAP_LITE_OCR_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to add OCR toolbar action")
endif()

set(SNAPLITE_SNIP_SOURCE "${SNAPLITE_GENERATED_SRC}/snip_window.cpp")

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
    src/ocr.cpp
    src/ocr_result_panel.cpp
    src/pin_window.cpp
    ${SNAPLITE_SNIP_SOURCE}
    src/snip_text_overlay.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
)

target_include_directories(SnapLite PRIVATE
    ${SNAPLITE_GENERATED_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# C++/WinRT's current Windows SDK coroutine support targets standard C++20.
target_compile_features(SnapLite PRIVATE cxx_std_20)
target_compile_definitions(
    SnapLite PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SNAP_LITE_VERSION=L"${PROJECT_VERSION}"
)
target_link_libraries(
    SnapLite PRIVATE
    user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi imm32 windowsapp
)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive- /utf-8)
    set_property(
        SOURCE ${SNAPLITE_SNIP_SOURCE}
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${SNAPLITE_GENERATED_SRC}/window_bounds_fix.h"
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/drag_repaint_fix.h"
    )
    set_property(
        SOURCE src/ocr.cpp
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/ocr_winrt_compat.h"
    )
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)
