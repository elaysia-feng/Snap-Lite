cmake_minimum_required(VERSION 3.20)
project(SnapLite VERSION 1.3.1 LANGUAGES CXX RC)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/SnapLite.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
    @ONLY
)

add_executable(SnapLite WIN32
    src/main.cpp
    src/app.cpp
    src/editor_toolbar.cpp
    src/capture.cpp
    src/capture_settings.cpp
    src/editor_window.cpp
    src/pin_window.cpp
    src/snip_window.cpp
    src/snip_text_overlay.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
)

target_include_directories(SnapLite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_features(SnapLite PRIVATE cxx_std_17)
target_compile_definitions(
    SnapLite PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SNAP_LITE_VERSION=L"${PROJECT_VERSION}"
)
target_link_libraries(
    SnapLite PRIVATE
    user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi
)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive- /utf-8)
    set_property(
        SOURCE src/snip_window.cpp
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/window_bounds_fix.h"
    )
    set_property(
        SOURCE src/editor_toolbar.cpp
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/toolbar_icon_render.h"
    )
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)
