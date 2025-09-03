# Chromedirect CEF + OpenVR Overlay

An example that embeds a Chromium Embedded Framework (CEF) off-screen rendered browser into an OpenVR overlay. It demonstrates overlay creation and texture submission via DXGI shared handles.

## Features
- CEF Off-Screen Rendering (GPU via ANGLE D3D11)
- Presenter abstraction (`src/Presenter.h`) with two implementations:
  - `OpenVRPresenter`: submits frames to an OpenVR overlay
  - `D3DPresenter`: simple desktop D3D11 presenter (windowed)
- DXGI shared-handle texture submission (legacy handle) to the overlay

## Prerequisites
- Windows 10/11 (x64)
- Visual Studio 2022 (with Desktop C++)
- CMake >= 3.20
- A GPU supporting D3D11
- SteamVR (OpenVR runtime) installed and running for VR mode
- CEF minimal binary distribution (e.g. cef_binary_139.x_windows64_minimal)

Optional (if not using the default CEF path hardcoded in CMake):
- Set the environment variable `CEF_ROOT` to your extracted CEF directory, or pass `-DCEF_ROOT=...` to CMake.

## Build
Example Release build using VS 2022 generator:
```
# Optionally set CEF_ROOT if not using the default SDK path inside the repo
# set CEF_ROOT=C:\path\to\cef_binary_139.x_windows64_minimal

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
Artifacts are placed under `build/bin/` alongside all required CEF and OpenVR runtime files (copied by CMake).

## Run
1. Start SteamVR.
2. Run `build/bin/chromedirect_demo.exe`.
3. By default, the app starts in VR overlay mode (headless window). To use the desktop presenter instead, set `ENABLE_VR_MODE = false` in `src/main.cpp` and rebuild.

The overlay shows a live CEF browser (default URL: Google). 

## Logging
- Console logs from the main process
- CEF logs at `build/bin/cef_detailed.log`