cmake_minimum_required(VERSION 3.21)
project(screenshare CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg.json에 적힌 라이브러리 (WebRTC 전송, JSON)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)

add_executable(screenshare
    src/main.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/Scaler.cpp
    src/Capture.cpp
    src/CaptureSources.cpp
    src/Encoder.cpp
    src/Pipeline.cpp
    src/PreviewShare.cpp
    src/Streamer.cpp
    src/AudioCapture.cpp
)
target_compile_options(screenshare PRIVATE /utf-8)

target_compile_definitions(screenshare PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(screenshare PRIVATE third_party/nvenc)
target_link_libraries(screenshare PRIVATE
    d3d11 dxgi d3dcompiler dwmapi winmm windowsapp ole32 mmdevapi
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    Opus::opus
)
