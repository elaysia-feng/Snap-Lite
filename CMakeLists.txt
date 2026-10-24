cmake_minimum_required(VERSION 3.20)
project(SnapLite VERSION 1.7.0 LANGUAGES CXX RC)

# 部分中文 Windows 环境下，CMake 对 MSVC /showIncludes 前缀的探测会解码错误，
# Ninja 随后无法记录头文件依赖，改头文件时可能跳过依赖它的 C++ 编译单元。
# 本地构建入口在中文 PowerShell 环境中传入已验证的前缀来修正这条依赖链。
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja" AND
   DEFINED SNAPLITE_MSVC_SHOWINCLUDES_PREFIX)
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${SNAPLITE_MSVC_SHOWINCLUDES_PREFIX}")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${SNAPLITE_MSVC_SHOWINCLUDES_PREFIX}")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# 生成管线是构建的一等依赖：脚本或模板源码一旦变化必须触发重新配置。
# 否则 `cmake --build` 会沿用上一次 configure 生成的旧产物，
# 表现为"改了代码没反应"（改进审计报告 P0-2）。
set(SNAP_LITE_GENERATION_INPUTS
    scripts/normalize_ico.ps1
    scripts/generate_eraser_history_sources.py
    scripts/apply_eraser_history_patch.py
    scripts/generate_anime_toolbar_source.py
    scripts/add_toolbar_tooltips.py
    scripts/add_ocr_action.py
    scripts/apply_unified_toolbar_theme.py
    scripts/generate_unified_ui_sources.py
    src/editor_toolbar.cpp
    src/snip_window.cpp
    src/snip_window.h
    src/snip_window_original.inc
    src/pin_window.cpp
    src/ocr_result_panel_v2.cpp
    src/toolbar_icon_render_gdi.h
    src/toolbar_icon_render_gdi_anime.h
    src/edit_repaint_fix.h
    src/window_bounds_fix.h
    resources/SnapLite.ico
    resources/k_on_five_chibi.png
    resources/toolbar_function_mascots.png
)
foreach(snaplite_generation_input IN LISTS SNAP_LITE_GENERATION_INPUTS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${snaplite_generation_input}")
endforeach()

set(SNAPLITE_ICON "${CMAKE_CURRENT_BINARY_DIR}/SnapLite.ico")
# VERSIONINFO 需要逗号分隔的四段式版本号
set(SNAP_LITE_VERSION_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
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

# Undo/redo memory policy (two hot raw HBITMAP snapshots plus lossless PNG cold
# storage, 128 entries / 64MB per stack) now lives directly in
# src/snip_window.h, so the generated copy and the repository copy are
# byte-identical and the two Tool enum variants can no longer diverge.

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

execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/apply_unified_toolbar_theme.py"
            "${SNAPLITE_GENERATED_SRC}/toolbar_icon_render_gdi_anime.h"
    RESULT_VARIABLE SNAP_LITE_TOOLBAR_THEME_RESULT
)
if(NOT SNAP_LITE_TOOLBAR_THEME_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply shared toolbar theme")
endif()

# All custom popup surfaces share one visual language. System-owned file
# pickers remain native Windows dialogs, but Snap-Lite-owned controls use the
# same cream/pink palette, corner radii, typography, and interaction colors.
set(SNAPLITE_UI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_ui")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_unified_ui_sources.py"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${SNAPLITE_UI_GENERATED_DIR}"
    RESULT_VARIABLE SNAP_LITE_UI_GENERATE_RESULT
)
if(NOT SNAP_LITE_UI_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate unified UI surfaces")
endif()

set(SNAPLITE_SNIP_SOURCE "${SNAPLITE_GENERATED_SRC}/snip_window.cpp")
set(SNAPLITE_PIN_SOURCE "${SNAPLITE_UI_GENERATED_DIR}/pin_window.cpp")
set(SNAPLITE_OCR_PANEL_SOURCE "${SNAPLITE_UI_GENERATED_DIR}/ocr_result_panel_v2.cpp")

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
    src/delay_overlay.cpp
    src/update_checker.cpp
    src/ocr_v2.cpp
    ${SNAPLITE_OCR_PANEL_SOURCE}
    ${SNAPLITE_PIN_SOURCE}
    ${SNAPLITE_SNIP_SOURCE}
    src/snip_text_overlay.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/SnapLite.rc
)

target_include_directories(SnapLite PRIVATE
    ${SNAPLITE_GENERATED_SRC}
    ${SNAPLITE_UI_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# C++/WinRT's current Windows SDK coroutine support targets standard C++20.
target_compile_features(SnapLite PRIVATE cxx_std_20)
target_compile_definitions(
    SnapLite PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    SNAP_LITE_VERSION=L\"${PROJECT_VERSION}\"
)
target_link_libraries(
    SnapLite PRIVATE
    user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi imm32 windowsapp winhttp bcrypt
)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive- /utf-8)
    set_property(
        SOURCE ${SNAPLITE_SNIP_SOURCE}
        APPEND PROPERTY COMPILE_OPTIONS
        "/FI${SNAPLITE_GENERATED_SRC}/window_bounds_fix.h"
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/drag_repaint_fix.h"
    )
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)

# 可单独运行真实窗口交互回归；测试替身仅隔离未调用的系统 OCR 入口。
option(SNAPLITE_BUILD_INTERACTION_TESTS "Build native screenshot interaction tests" OFF)
if(SNAPLITE_BUILD_INTERACTION_TESTS)
    enable_testing()
    add_executable(SnapLiteInteractionTests
        tests/snip_interaction_test.cpp
        ${SNAPLITE_SNIP_SOURCE}
        ${SNAPLITE_EDITOR_SOURCE}
        src/capture.cpp
        src/capture_settings.cpp
        src/snip_text_overlay.cpp
    )
    target_include_directories(SnapLiteInteractionTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,INCLUDE_DIRECTORIES>)
    target_compile_definitions(SnapLiteInteractionTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,COMPILE_DEFINITIONS>
        # 测试钩子（历史长度的上限覆盖）只编进测试二进制。产品目标
        # SnapLite 不定义这个宏：为测试而存在的开关不该留在产品上。
        SNAPLITE_EDIT_TEST_HOOKS
    )
    target_compile_options(SnapLiteInteractionTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,COMPILE_OPTIONS>)
    target_compile_features(SnapLiteInteractionTests PRIVATE cxx_std_20)
    target_link_libraries(SnapLiteInteractionTests PRIVATE
        user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi imm32)
    add_test(NAME snip_interaction COMMAND SnapLiteInteractionTests "${CMAKE_CURRENT_BINARY_DIR}")

    # 延时覆盖层此前只有源码级静态守卫，"倒计时归零后截图里会不会残留
    # 覆盖层"这种取决于桌面重绘真实时序的问题断不了。这里给它一份真正
    # 跑起来的运行时回归。
    add_executable(SnapLiteDelayOverlayTests
        tests/delay_overlay_test.cpp
        src/delay_overlay.cpp
        src/capture.cpp
        src/capture_settings.cpp
    )
    target_include_directories(SnapLiteDelayOverlayTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,INCLUDE_DIRECTORIES>)
    target_compile_definitions(SnapLiteDelayOverlayTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,COMPILE_DEFINITIONS>
        # 测试钩子（覆盖层颜色、基线、等待决策的只读计数器）只编进测试
        # 二进制。产品目标 SnapLite 不定义这个宏：为测试而存在的公开接口
        # 不该留在产品上，而"产品路径到底有没有真的等"必须靠这些钩子才
        # 问得出来。
        SNAPLITE_DELAY_TEST_HOOKS
    )
    target_compile_options(SnapLiteDelayOverlayTests PRIVATE
        $<TARGET_PROPERTY:SnapLite,COMPILE_OPTIONS>)
    target_compile_features(SnapLiteDelayOverlayTests PRIVATE cxx_std_20)
    target_link_libraries(SnapLiteDelayOverlayTests PRIVATE
        user32 gdi32 gdiplus shell32 ole32 uuid advapi32 comdlg32 comctl32 dwmapi imm32)
    add_test(NAME delay_overlay COMMAND SnapLiteDelayOverlayTests)
    # 约定退出码 77 = 跳过。没有可抓屏的交互式桌面时（GitHub 的
    # windows-latest 跑在会话 0 的服务 window station）这个用例测不了，
    # 测了必然红——会把 PR 和发布一起挡住。
    set_tests_properties(delay_overlay PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(SnapLiteUpdateVersionTests tests/update_version_test.cpp)
    target_include_directories(SnapLiteUpdateVersionTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(SnapLiteUpdateVersionTests PRIVATE cxx_std_20)
    add_test(NAME update_version COMMAND SnapLiteUpdateVersionTests)

    # 生成管线的源码级守卫与产物内容校验必须进本地构建入口：否则
    # "本地绿灯"并不代表生成产物是对的（这正是 P1-21 那个盲区）。
    add_test(
        NAME build_scripts
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
        NAME verify_generated_sources
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_generated_sources.py"
                "${CMAKE_CURRENT_BINARY_DIR}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
