cmake_minimum_required(VERSION 3.20)
project(YAZDTrainer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_dx11.cpp
    third_party/imgui/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX IMGUI_DISABLE_DEMO_WINDOWS)

add_executable(YAZDTrainer WIN32
    src/main.cpp src/game.cpp src/mono.cpp src/mem.cpp src/log.cpp src/app.rc)
target_include_directories(YAZDTrainer PRIVATE src)
target_compile_options(YAZDTrainer PRIVATE /W3 /permissive- /utf-8)
target_compile_definitions(YAZDTrainer PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(YAZDTrainer PRIVATE imgui d3d11 dxgi dwmapi psapi shell32)
