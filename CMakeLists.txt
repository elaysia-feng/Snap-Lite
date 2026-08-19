cmake_minimum_required(VERSION 3.20)
project(SnapLite LANGUAGES CXX)

add_executable(SnapLite WIN32
    src/main.cpp
)

target_compile_features(SnapLite PRIVATE cxx_std_17)
target_compile_definitions(SnapLite PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(SnapLite PRIVATE <windowsx.h> <cstdlib>)
target_link_libraries(SnapLite PRIVATE user32 gdi32 gdiplus shell32 ole32)

if(MSVC)
    target_compile_options(SnapLite PRIVATE /W4 /permissive-)
endif()

install(TARGETS SnapLite RUNTIME DESTINATION .)
