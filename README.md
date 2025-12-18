# Chromedirect CEF + OpenVR Overlay

An example that embeds a Chromium Embedded Framework (CEF) off-screen rendered browser into an OpenVR overlay. It demonstrates overlay creation and texture submission via DXGI shared handles.

## Features
- CEF Off-Screen Rendering (GPU via ANGLE D3D11)
- Presenter abstraction (`src/Presenter.h`) with two implementations:
  - `OpenVRPresenter`: submits frames to an OpenVR overlay
  - `D3DPresenter`: simple desktop D3D11 presenter (windowed)
- DXGI shared-handle texture submission (legacy handle) to the overlay

## Prerequisites
- Windows 10/11 (x64) with a GPU that supports D3D11
- Zig 0.15.2 or newer
- SteamVR (OpenVR runtime) installed and running for VR mode
- Deno (only if you plan to use the launcher script)

The Zig build downloads the requested CEF SDK on demand via the `cefzig` helper, so no manual SDK management or CMake install is required.

## Build
```
zig build          # compiles chromedirect_demo and stages runtime files under zig-out/bin
zig build run      # installs, copies CEF/OpenVR DLLs beside the exe, then runs it
```

Useful options:
- `-Dcef-version=142.5.0+142.0.17` – override the default CEF build.
- `-Dcef-base-url=https://…` – mirror URL if you host CEF elsewhere.
- `-Dsubsystem-windows=false` – keep a console window open (default hides it).

The first build downloads and extracts the CEF archive into `.zig-cache/cef-cache/cef_windows_x86_64`. Subsequent builds reuse the cache unless you delete it or change the requested version.

## Run
1. Start SteamVR.
2. Option A: Run `zig-out/bin/chromedirect_demo.exe` directly (VR overlay mode by default).
3. Option B: Use the Deno launcher to create the overlay and spawn the C++ host (recommended for scripting):

```
deno run -A deno_cef_overlay_launcher.ts \
  --key=cef.web.overlay \
  --width=1280 --height=720 \
  --scale=1.0 \
  --url=https://www.google.com \
  --exe=./zig-out/bin/chromedirect_demo.exe
```

The overlay shows a live CEF browser (default URL: Google).

## Logging
- Console logs from the main process
- CEF logs at `build/bin/cef_detailed.log`

## Notes
- The C++ host will find or create the overlay by key (`cef.web.overlay`) and submit textures to it. The Deno script pre-creates and positions the overlay, so the host picks it up immediately.
- You can customize overlay key, size, and URL via the Deno script flags.
