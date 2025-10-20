// Deno launcher: creates an OpenVR overlay and spawns the C++ CEF host
// Usage (from repo root):
//   deno run -A deno_cef_overlay_launcher.ts --key=cef.web.overlay --width=1280 --height=720 --scale=1.0 --url=https://www.google.com --exe=./build/bin/chromedirect_demo.exe

import * as OpenVR from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/openvr_bindings.ts";
import { P } from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/pointers.ts";
import {
  createStruct,
  stringToPointer,
} from "https://raw.githubusercontent.com/mommysgoodpuppy/OpenVR_TS_Bindings_Deno/refs/heads/main/utils.ts";

type Args = {
  key: string;
  width: number;
  height: number;
  scale: number;
  url: string;
  exe: string;
  vrMode: boolean;
  fps?: number;
};

function parseArgs(): Args {
  const defaults: Args = {
    key: "cef.web.overlay",
    width: 1600,
    height: 900,
    scale: 1.0,
    url: "",
    exe: Deno.build.os === "windows"
      ? ".\\build\\bin\\chromedirect_demo.exe"
      : "./build/bin/chromedirect_demo",
    vrMode: false,
  };
  const out: Args = { ...defaults };
  let widthExplicit = false;
  let heightExplicit = false;
  let fpsExplicit = false;
  let scaleExplicit = false;

  for (const rawArg of Deno.args) {
    if (!rawArg.startsWith("--")) continue;
    const eq = rawArg.indexOf("=");
    const k = eq === -1 ? rawArg.slice(2) : rawArg.slice(2, eq);
    const v = eq === -1 ? "" : rawArg.slice(eq + 1);
    switch (k) {
      case "key":
        out.key = v;
        break;
      case "width":
        out.width = Math.max(64, Number(v) || out.width);
        widthExplicit = true;
        break;
      case "height":
        out.height = Math.max(64, Number(v) || out.height);
        heightExplicit = true;
        break;
      case "scale":
        out.scale = Math.max(0.01, Number(v) || out.scale);
        scaleExplicit = true;
        break;
      case "url":
        out.url = v;
        break;
      case "exe":
        out.exe = v;
        break;
      case "fps":
        out.fps = Math.max(1, Number(v) || 120);
        fpsExplicit = true;
        break;
      case "vr-mode": {
        const val = v.toLowerCase();
        out.vrMode = v === ""
          ? true
          : !(val === "0" || val === "false" || val === "no");
        break;
      }
    }
  }
  if (out.vrMode) {
    if (!widthExplicit) out.width = 8000;
    if (!heightExplicit) out.height = 8000;
    if (!fpsExplicit) out.fps = 120;
    if (!scaleExplicit) out.scale = 3.0;
  } else {
    if (!fpsExplicit) out.fps = 60;
    if (!scaleExplicit) out.scale = 1.0;
  }
  // Default URL to local index.html via file:// if none provided
  if (!out.url) {
    const cwd = Deno.cwd();
    const sep = Deno.build.os === "windows" ? "\\" : "/";
    const path = `${cwd}${sep}index.html`;
    const fileUrl = Deno.build.os === "windows"
      ? `file:///${path.replace(/\\/g, "/")}`
      : `file://${path}`;
    out.url = fileUrl;
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
  if (
    e !== OpenVR.OverlayError.VROverlayError_None &&
    e !== OpenVR.OverlayError.VROverlayError_KeyInUse
  ) {
    throw new Error(`CreateOverlay failed: ${OpenVR.OverlayError[e]}`);
  }
  const overlayHandle = new Deno.UnsafePointerView(overlayHandlePTR)
    .getBigUint64();

  overlay.SetOverlayFlag(
    overlayHandle,
    OpenVR.OverlayFlags.VROverlayFlags_Panorama,
    false,
  );
  overlay.SetOverlayFlag(
    overlayHandle,
    OpenVR.OverlayFlags.VROverlayFlags_StereoPanorama,
    true,
  );
  overlay.SetOverlaySortOrder(overlayHandle, 9999); //we are privileged

  overlay.SetOverlayWidthInMeters(overlayHandle, scale);

  const idtransform: OpenVR.HmdMatrix34 = {
    m: [
      [1, 0, 0, 0],
      [0, 1, 0, 0],
      [0, 0, 1, -1],
    ],
  };
  const [transformptr, _transview] = createStruct<OpenVR.HmdMatrix34>(
    idtransform,
    OpenVR.HmdMatrix34Struct,
  );

  overlay.SetOverlayTransformTrackedDeviceRelative(
    overlayHandle,
    OpenVR.k_unTrackedDeviceIndex_Hmd,
    transformptr,
  );

  const bounds = { uMin: 0, uMax: 1, vMin: 0, vMax: 1 };
  const boundsBuf = new ArrayBuffer(OpenVR.TextureBoundsStruct.byteSize);
  OpenVR.TextureBoundsStruct.write(bounds, new DataView(boundsBuf));
  const boundsPtr = Deno.UnsafePointer.of(boundsBuf) as Deno.PointerValue<
    OpenVR.TextureBounds
  >;
  overlay.SetOverlayTextureBounds(overlayHandle, boundsPtr);
  overlay.ShowOverlay(overlayHandle);

  return { overlay, overlayHandle };
}

function spawnHost(exe: string, args: Args) {
  // Pass essential flags to the C++ host
  const params = [
    `--vr-mode=${args.vrMode ? "true" : "false"}`,
    `--width=${args.width}`,
    `--height=${args.height}`,
    `--scale=${args.scale}`,
    `--overlay-key=${args.key}`,
    `--url=${args.url}`,
    ...(args.fps ? [`--fps=${args.fps}`] : []),
  ];
  const cmd = new Deno.Command(exe, {
    args: params,
    stdout: "piped",
    stderr: "piped",
  });
  const child = cmd.spawn();
  (async () => {
    const r = child.stdout.getReader();
    const _dec = new TextDecoder();
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
  if (args.vrMode) {
    console.log("[Deno] VR overlay mode:", args);
    console.log(await OpenVR.initializeOpenVR());
    const { overlay, overlayHandle } = initOverlay(args.key, args.scale);
    console.log(
      `[Deno] Overlay ready: key=${args.key}, handle=${overlayHandle}`,
    );

    const child = await spawnHost(args.exe, args);
    console.log(`[Deno] Spawned host: pid=${child.pid}`);
    try {
      overlay.SetOverlayRenderingPid(overlayHandle, child.pid);
      console.log(`[Deno] SetOverlayRenderingPid -> ${child.pid}`);
    } catch (e) {
      console.warn("[Deno] Failed to SetOverlayRenderingPid:", e);
    }

    try {
      let running = true;
      addEventListener("SIGINT", () => {
        running = false;
        try {
          child.kill("SIGTERM");
        } catch { /* ignore */ }
      });
      const start = performance.now();
      while (running) {
        const _now = performance.now();
        const _t = (_now - start) / 1000;
        /* try {
          setOverlayTransformAnimated(overlay, overlayHandle, t);
        } catch (e) {
          console.warn("[Deno] Failed to animate overlay transform:", e);
        } */
        overlay.WaitFrameSync(100);
      }
    } finally {
      try {
        child.kill("SIGKILL");
      } catch { /* ignore */ }
    }
  } else {
    console.log("[Deno] Desktop mode (no OpenVR overlay):", args);
    const child = await spawnHost(args.exe, args);
    addEventListener("SIGINT", () => {
      try {
        child.kill("SIGTERM");
      } catch { /* ignore */ }
    });
    const status = await child.status;
    console.log(
      `[Deno] Host exited: code=${status.code}, signal=${
        status.signal ?? "none"
      }`,
    );
  }
}
