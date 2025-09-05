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
2. Option A: Run `build/bin/chromedirect_demo.exe` directly (VR overlay mode by default).
3. Option B: Use the Deno launcher to create the overlay and spawn the C++ host (recommended for scripting):

```
deno run -A deno_cef_overlay_launcher.ts \
  --key=cef.web.overlay \
  --width=1280 --height=720 \
  --scale=1.0 \
  --url=https://www.google.com \
  --exe=./build/bin/chromedirect_demo.exe
```

The overlay shows a live CEF browser (default URL: Google).

## Logging
- Console logs from the main process
- CEF logs at `build/bin/cef_detailed.log`

## Notes
- The C++ host will find or create the overlay by key (`cef.web.overlay`) and submit textures to it. The Deno script pre-creates and positions the overlay, so the host picks it up immediately.
- You can customize overlay key, size, and URL via the Deno script flags.
