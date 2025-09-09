// Deno launcher: creates an OpenVR overlay and spawns the C++ CEF host
// Usage (from repo root):
//   deno run -A deno_cef_overlay_launcher.ts --key=cef.web.overlay --width=1280 --height=720 --scale=1.0 --url=https://www.google.com --exe=./build/bin/chromedirect_demo.exe

import * as OpenVR from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/openvr_bindings.ts";
import { P } from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/pointers.ts";
import { stringToPointer, createStruct } from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/utils.ts";

console.log(await OpenVR.initializeOpenVR())

type Args = {
  key: string;
  width: number;
  height: number;
  scale: number;
  url: string;
  exe: string;
  fps?: number;
  shaderPanorama?: boolean;
  stereoPanoramaFlag?: boolean;
  fovDeg?: number;
  shaderDebug?: string;
  debugIwerHeartbeat?: number; // ms; if set, enables heartbeat
  disableIwerExtension?: boolean; // if true, do not inject iwer extension
  v8Ping?: number; // ms; if set, pings page via ExecuteJavaScript
};

function parseArgs(): Args {
  const defaults: Args = {
    key: "cef.web.overlay",
    width: 4096,
    height: 4096,
    scale: 3.0,
    url: "",
    exe: Deno.build.os === "windows" ? ".\\build\\bin\\chromedirect_demo.exe" : "./build/bin/chromedirect_demo",
  };
  const out = { ...defaults };
  for (const a of Deno.args) {
    const m = a.match(/^--([^=]+)=(.*)$/);
    if (!m) continue;
    const k = m[1];
    const v = m[2];
    switch (k) {
      case "key": out.key = v; break;
      case "width": out.width = Math.max(64, Number(v) || out.width); break;
      case "height": out.height = Math.max(64, Number(v) || out.height); break;
      case "scale": out.scale = Math.max(0.01, Number(v) || out.scale); break;
      case "url": out.url = v; break;
      case "exe": out.exe = v; break;
      case "fps": out.fps = Math.max(1, Number(v) || 120); break;
      case "shader-panorama": out.shaderPanorama = v === "true" || v === "1"; break;
      case "overlay-stereo-panorama": out.stereoPanoramaFlag = v === "true" || v === "1"; break;
      case "fov-deg": out.fovDeg = Math.max(1, Number(v) || 90); break;
      case "shader-debug": out.shaderDebug = v; break;
      case "debug-iwer-heartbeat": out.debugIwerHeartbeat = Math.max(50, Number(v) || 2000); break;
      case "disable-iwer-extension": out.disableIwerExtension = (v === "1" || v === "true"); break;
      case "v8-ping": out.v8Ping = Math.max(50, Number(v) || 1000); break;
    }
  }
  // Default URL to local index.html via file:// if none provided
  if (!out.url) {
    const cwd = Deno.cwd();
    const sep = Deno.build.os === "windows" ? "\\" : "/";
    const path = `${cwd}${sep}index.html`;
    const fileUrl = Deno.build.os === "windows" ? `file:///${path.replace(/\\/g, "/")}` : `file://${path}`;
    out.url = fileUrl;
  }
  // Default heartbeat ON at 2000ms if not specified
  if (!("debugIwerHeartbeat" in out)) {
    out.debugIwerHeartbeat = 2000;
  }
  return out;
}

function initOverlay(key: string, scale: number) {
  const errorX = Deno.UnsafePointer.of(new Int32Array(1))!;
  OpenVR.VR_InitInternal(errorX, OpenVR.ApplicationType.VRApplication_Overlay);
  const err = new Deno.UnsafePointerView(errorX).getInt32();
  if (err !== 0) throw new Error(`VR_InitInternal failed: ${err}`);

  const initErrorPtr = P.Int32P<OpenVR.InitError>();
  const overlayPTR = OpenVR.VR_GetGenericInterface(
    stringToPointer(OpenVR.IVROverlay_Version),
    initErrorPtr,
  );
  const overlay = new OpenVR.IVROverlay(overlayPTR);
  if (!overlay) throw new Error("Failed to get IVROverlay interface");

  const overlayHandlePTR = P.BigUint64P<OpenVR.OverlayHandle>();
  const e = overlay.CreateOverlay(key, "CEF Web Overlay XDS", overlayHandlePTR);
  if (e !== OpenVR.OverlayError.VROverlayError_None && e !== OpenVR.OverlayError.VROverlayError_KeyInUse) {
    throw new Error(`CreateOverlay failed: ${OpenVR.OverlayError[e]}`);
  }
  const overlayHandle = new Deno.UnsafePointerView(overlayHandlePTR).getBigUint64();

  overlay.SetOverlayFlag(overlayHandle, OpenVR.OverlayFlags.VROverlayFlags_Panorama, false)
  overlay.SetOverlayFlag(overlayHandle, OpenVR.OverlayFlags.VROverlayFlags_StereoPanorama, true)
  overlay.SetOverlaySortOrder(overlayHandle, 9999) //we are privileged

  overlay.SetOverlayWidthInMeters(overlayHandle, scale);

  const idtransform: OpenVR.HmdMatrix34 = {
    m: [
      [1, 0, 0, 0],
      [0, 1, 0, 0],
      [0, 0, 1, -1]
    ]
  }
  const [transformptr, _transview] = createStruct<OpenVR.HmdMatrix34>(idtransform, OpenVR.HmdMatrix34Struct)

  overlay.SetOverlayTransformTrackedDeviceRelative(overlayHandle, OpenVR.k_unTrackedDeviceIndex_Hmd, transformptr)

  const bounds = { uMin: 0, uMax: 1, vMin: 0, vMax: 1 };
  const boundsBuf = new ArrayBuffer(OpenVR.TextureBoundsStruct.byteSize);
  OpenVR.TextureBoundsStruct.write(bounds, new DataView(boundsBuf));
  const boundsPtr = Deno.UnsafePointer.of(boundsBuf) as Deno.PointerValue<OpenVR.TextureBounds>;
  overlay.SetOverlayTextureBounds(overlayHandle, boundsPtr);
  overlay.ShowOverlay(overlayHandle);

  return { overlay, overlayHandle };
}

function setOverlayTransformAnimated(
  overlay: OpenVR.IVROverlay,
  overlayHandle: OpenVR.OverlayHandle,
  tSeconds: number,
) {
  // Simple lateral oscillation with slight yaw over time
  const amp = 0.15; // meters
  const x = Math.sin(tSeconds) * amp;
  const y = 1.0; // constant height
  const z = -2.0; // in front of user
  const yaw = Math.sin(tSeconds * 0.5) * 0.25; // radians, small yaw

  const cy = Math.cos(yaw);
  const sy = Math.sin(yaw);

  const transform: OpenVR.HmdMatrix34 = {
    // Row-major 3x4: basis vectors + translation
    m: [
      [ cy, 0.0,  sy,  x ],
      [0.0, 1.0, 0.0,  y ],
      [-sy, 0.0,  cy,  z ],
    ],
  };

  const buf = new ArrayBuffer(OpenVR.HmdMatrix34Struct.byteSize);
  OpenVR.HmdMatrix34Struct.write(transform, new DataView(buf));
  const ptr = Deno.UnsafePointer.of<OpenVR.HmdMatrix34>(buf)!;
  overlay.SetOverlayTransformAbsolute(
    overlayHandle,
    OpenVR.TrackingUniverseOrigin.TrackingUniverseStanding,
    ptr,
  );
}

async function spawnHost(exe: string, args: Args) {
  // C++ currently uses defaults; we still pass helpful flags for future-proofing
  const params = [
    `--vr=true`,
    `--width=${args.width}`,
    `--height=${args.height}`,
    `--scale=${args.scale}`,
    `--overlay-key=${args.key}`,
    `--url=${args.url}`,
    `--debug-iwer-heartbeat=${args.debugIwerHeartbeat}`,
    ...(args.disableIwerExtension ? ["--disable-iwer-extension"] : []),
    ...(args.v8Ping ? [`--v8-ping=${args.v8Ping}`] : []),
    ...(args.fps ? [`--fps=${args.fps}`] : []),
    ...(args.shaderPanorama ? ["--shader-panorama=true"] : []),
    ...(args.stereoPanoramaFlag ? ["--overlay-stereo-panorama=true"] : []),
    ...(args.fovDeg ? [`--fov-deg=${args.fovDeg}`] : []),
    ...(args.shaderDebug ? [`--shader-debug=${args.shaderDebug}`] : []),
    `--log-console`,
  ];
  const cmd = new Deno.Command(exe, { args: params, stdout: "piped", stderr: "piped" });
  const child = cmd.spawn();
  (async () => {
    const r = child.stdout.getReader();
    const dec = new TextDecoder();
    while (true) {
      const { value, done } = await r.read();
      if (done) break;
      if (value) Deno.stdout.write(value);
    }
  })();
  (async () => {
    const r = child.stderr.getReader();
    while (true) {
      const { value, done } = await r.read();
      if (done) break;
      if (value) Deno.stderr.write(value);
    }
  })();
  return child;
}

if (import.meta.main) {
  const args = parseArgs();
  console.log("[Deno] Initializing OpenVR overlay:", args);
  const { overlay, overlayHandle } = initOverlay(args.key, args.scale);
  console.log(`[Deno] Overlay ready: key=${args.key}, handle=${overlayHandle}`);

  const child = await spawnHost(args.exe, args);
  console.log(`[Deno] Spawned host: pid=${child.pid}`);
  try {
    // Allow the spawned C++ process to render to this overlay
    overlay.SetOverlayRenderingPid(overlayHandle, child.pid);
    console.log(`[Deno] SetOverlayRenderingPid -> ${child.pid}`);
  } catch (e) {
    console.warn("[Deno] Failed to SetOverlayRenderingPid:", e);
  }

  // Keep process alive; pace with WaitFrameSync
  try {
    let running = true;
    addEventListener("SIGINT", () => { running = false; try { child.kill("SIGTERM"); } catch { /* ignore */ } });
    const start = performance.now();
    while (running) {
      const now = performance.now();
      const t = (now - start) / 1000;
      // Animate overlay transform slightly each tick
      /* try {
        setOverlayTransformAnimated(overlay, overlayHandle, t);
      } catch (e) {
        console.warn("[Deno] Failed to animate overlay transform:", e);
      } */
      overlay.WaitFrameSync(100);
    }
  } finally {
    try { child.kill("SIGKILL"); } catch { /* ignore */ }
  }
}
