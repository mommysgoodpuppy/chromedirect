#include "RendererBridge.h"

#include <include/cef_app.h>
#include <include/cef_v8.h>
#include <include/cef_command_line.h>
#include <include/wrapper/cef_helpers.h>
#include <cstring>
#include <iostream>

namespace {
// Inject a proxy that captures the created XRDevice instance and exposes a bridge.
const char* kBridgeJS = R"JS(
(function(){
  var w = (typeof window !== 'undefined') ? window : this;
  function tryWrapIWER(){
    try {
      if (!w) return false;
      if (!w.__iwerLogOnce) { try { console.log('[iwerBridge] init'); } catch(e){} w.__iwerLogOnce = true; }
      if (w.IWER && w.IWER.XRDevice && !w.IWER.__wrapped) {
        const Orig = w.IWER.XRDevice;
        w.IWER.XRDevice = new Proxy(Orig, {
          construct(target, args, newTarget){
            const inst = Reflect.construct(target, args, newTarget);
            w.__iwerDevice = inst;
            try { console.log('[iwerBridge] XRDevice constructed; controllers:', Object.keys(inst.controllers||{})); } catch(e){}
            return inst;
          }
        });
        w.IWER.__wrapped = true;
        try { console.log('[iwerBridge] XRDevice class wrapped'); } catch(e){}
        return true;
      }
    } catch(e){}
    return false;
  }
  function ensureBridge(){
    try {
      if (!w.iwerBridge) {
        w.iwerBridge = {
          applyPose: function(state){
            var dev = w.__iwerDevice;
            if (!dev) return false;
            try {
              if (!w.__iwerApplyCount) w.__iwerApplyCount = 0;
              if (state.hmd) {
                if (state.hmd.pos) dev.position.set(state.hmd.pos[0], state.hmd.pos[1], state.hmd.pos[2]);
                if (state.hmd.quat) dev.quaternion.set(state.hmd.quat[0], state.hmd.quat[1], state.hmd.quat[2], state.hmd.quat[3]);
              }
              if (dev.controllers) {
                var L = dev.controllers['left'];
                var R = dev.controllers['right'];
                if (L && state.left) {
                  if (state.left.pos) L.position.set(state.left.pos[0], state.left.pos[1], state.left.pos[2]);
                  if (state.left.quat) L.quaternion.set(state.left.quat[0], state.left.quat[1], state.left.quat[2], state.left.quat[3]);
                }
                if (R && state.right) {
                  if (state.right.pos) R.position.set(state.right.pos[0], state.right.pos[1], state.right.pos[2]);
                  if (state.right.quat) R.quaternion.set(state.right.quat[0], state.right.quat[1], state.right.quat[2], state.right.quat[3]);
                }
              }
              w.__iwerApplyCount++;
              if (w.__iwerApplyCount === 1 || (w.__iwerApplyCount % 120) === 0) {
                try {
                  console.log('[iwerBridge] applyPose count=' + w.__iwerApplyCount + ' hmd=', state.hmd && state.hmd.pos);
                  if (dev && dev.position) console.log('[iwerBridge] xrDevice.position =', dev.position.x, dev.position.y, dev.position.z);
                } catch(e){}
              }
              // Update the page title periodically so the browser process sees a signal even if console is suppressed.
              try {
                if (typeof document !== 'undefined' && (w.__iwerApplyCount % 30) === 0) {
                  document.title = 'iwerBridge applied #' + w.__iwerApplyCount;
                }
              } catch(e){}
              return true;
            } catch(e) { return false; }
          }
        };
      }
    } catch(e){}
  }
  function install(){
    tryWrapIWER();
    ensureBridge();
  }
  // Initial attempt and periodic retry in case IWER loads late
  install();
  var attempts = 0;
  var timer = setInterval(function(){
    if (tryWrapIWER()) {
      clearInterval(timer);
    }
    attempts++;
    if (attempts > 60) { // ~30s at 500ms
      clearInterval(timer);
    }
  }, 500);
  if (document && document.readyState !== 'complete' && typeof window !== 'undefined') {
    window.addEventListener('load', install, { once: true });
  }
})();
)JS";
}

void RendererBridge::OnWebKitInitialized() {
  // Renderer process: JS bridge registration
  CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
  const bool disabled = cmd.get() && cmd->HasSwitch("disable-iwer-extension");
  if (disabled) {
    std::cout << "[RendererBridge] IWER extension injection disabled via switch" << std::endl;
    return;
  }
  std::cout << "[RendererBridge] OnWebKitInitialized: registering bridge" << std::endl;
  InstallBridgeScript();
}

void RendererBridge::InstallBridgeScript() {
  CefRegisterExtension("v8/iwer_bridge", kBridgeJS, nullptr);
}

void RendererBridge::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefV8Context> context) {
  // Ensure iwerBridge exists even if the extension injection missed, unless disabled.
  CEF_REQUIRE_RENDERER_THREAD();
  CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
  const bool disabled = cmd.get() && cmd->HasSwitch("disable-iwer-extension");
  if (disabled) {
    return;
  }
  if (!context.get()) return;
  if (!context->Enter()) return;
  CefRefPtr<CefV8Value> global = context->GetGlobal();
  CefRefPtr<CefV8Value> existing = global->GetValue("iwerBridge");
  if (!existing.get() || !existing->IsObject()) {
    const char* kFallback = R"( (function(){
      var w = (typeof window !== 'undefined') ? window : this;
      if (!w.iwerBridge) {
        w.iwerBridge = {
          applyPose: function(state){
            try {
              if (!w.__iwerApplyCount) w.__iwerApplyCount = 0;
              w.__iwerApplyCount++;
              console.log('[iwerBridge:fallback] received state, count=', w.__iwerApplyCount, state && state.hmd && state.hmd.pos);
              if (w.__iwerDevice && w.__iwerDevice.position) {
                if (state && state.hmd && state.hmd.pos) w.__iwerDevice.position.set(state.hmd.pos[0], state.hmd.pos[1], state.hmd.pos[2]);
                if (state && state.hmd && state.hmd.quat) w.__iwerDevice.quaternion.set(state.hmd.quat[0], state.hmd.quat[1], state.hmd.quat[2], state.hmd.quat[3]);
                console.log('[iwerBridge:fallback] xrDevice.position =', w.__iwerDevice.position.x, w.__iwerDevice.position.y, w.__iwerDevice.position.z);
              }
              return true;
            } catch(e){ try{ console.warn('[iwerBridge:fallback] error', e); }catch(_e){} return false; }
          }
        };
      }
    })(); )";
    CefRefPtr<CefV8Value> retval;
    CefRefPtr<CefV8Exception> exc;
    context->Eval(kFallback, CefString(), 0, retval, exc);
  }
  context->Exit();
}

bool RendererBridge::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                              CefRefPtr<CefFrame> frame,
                                              CefProcessId source_process,
                                              CefRefPtr<CefProcessMessage> message) {
  // Runs on renderer process main thread.
  const CefString& name = message->GetName();
  if (name != "VR_STATE") return false;
  std::cout << "[RendererBridge] VR_STATE received" << std::endl;

  auto args = message->GetArgumentList();
  if (!args.get() || args->GetSize() < 1 || args->GetType(0) != VTYPE_BINARY) return false;
  CefRefPtr<CefBinaryValue> bin = args->GetBinary(0);
  size_t len = bin->GetSize();
  if (len < sizeof(float) * (3+4)*3) return false;
  std::vector<char> buf(len);
  bin->GetData(buf.data(), len, 0);
  const float* pf = reinterpret_cast<const float*>(buf.data());

  const float* hmd_pos   = pf + 0;       // 3
  const float* hmd_quat  = pf + 3;       // 4
  const float* left_pos  = pf + 7;       // 3
  const float* left_quat = pf + 10;      // 4
  const float* right_pos = pf + 14;      // 3
  const float* right_quat= pf + 17;      // 4

  CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
  if (!ctx.get()) return false;
  if (!ctx->Enter()) return false;
  bool ok = ApplyPoseToPage(ctx, hmd_pos, hmd_quat, left_pos, left_quat, right_pos, right_quat);
  ctx->Exit();
  return ok;
}

bool RendererBridge::ApplyPoseToPage(CefRefPtr<CefV8Context> context,
                                     const float* hmd_pos,
                                     const float* hmd_quat,
                                     const float* left_pos,
                                     const float* left_quat,
                                     const float* right_pos,
                                     const float* right_quat) {
  CefRefPtr<CefV8Value> global = context->GetGlobal();
  CefRefPtr<CefV8Value> bridge = global->GetValue("iwerBridge");
  if (!bridge.get()) {
    std::cout << "[RendererBridge] WARN: window.iwerBridge not found in page context" << std::endl;
    return false;
  }
  CefRefPtr<CefV8Value> apply = bridge->GetValue("applyPose");
  if (!apply.get() || !apply->IsFunction()) {
    std::cout << "[RendererBridge] WARN: iwerBridge.applyPose missing or not a function" << std::endl;
    return false;
  }

  auto makeVec = [](const float* p3){
    CefRefPtr<CefV8Value> arr = CefV8Value::CreateArray(3);
    arr->SetValue(0, CefV8Value::CreateDouble(p3[0]));
    arr->SetValue(1, CefV8Value::CreateDouble(p3[1]));
    arr->SetValue(2, CefV8Value::CreateDouble(p3[2]));
    return arr;
  };
  auto makeQuat = [](const float* q4){
    CefRefPtr<CefV8Value> arr = CefV8Value::CreateArray(4);
    arr->SetValue(0, CefV8Value::CreateDouble(q4[0]));
    arr->SetValue(1, CefV8Value::CreateDouble(q4[1]));
    arr->SetValue(2, CefV8Value::CreateDouble(q4[2]));
    arr->SetValue(3, CefV8Value::CreateDouble(q4[3]));
    return arr;
  };

  CefRefPtr<CefV8Value> state = CefV8Value::CreateObject(nullptr, nullptr);
  {
    CefRefPtr<CefV8Value> hmd = CefV8Value::CreateObject(nullptr, nullptr);
    hmd->SetValue("pos", makeVec(hmd_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    hmd->SetValue("quat", makeQuat(hmd_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("hmd", hmd, V8_PROPERTY_ATTRIBUTE_NONE);
  }
  {
    CefRefPtr<CefV8Value> left = CefV8Value::CreateObject(nullptr, nullptr);
    left->SetValue("pos", makeVec(left_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    left->SetValue("quat", makeQuat(left_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("left", left, V8_PROPERTY_ATTRIBUTE_NONE);
  }
  {
    CefRefPtr<CefV8Value> right = CefV8Value::CreateObject(nullptr, nullptr);
    right->SetValue("pos", makeVec(right_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    right->SetValue("quat", makeQuat(right_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("right", right, V8_PROPERTY_ATTRIBUTE_NONE);
  }

  CefV8ValueList argv;
  argv.push_back(state);
  // Execute the function object with 'bridge' as the receiver (thisArg).
  CefRefPtr<CefV8Value> result = apply->ExecuteFunctionWithContext(context, bridge, argv);
  if (!result.get()) {
    std::cout << "[RendererBridge] ERROR: applyPose invocation failed (null result)" << std::endl;
    return false;
  }
  // Emit a console.log from page after applying to verify visibility.
  CefRefPtr<CefV8Value> global2 = context->GetGlobal();
  if (global2.get()) {
    CefRefPtr<CefV8Value> console = global2->GetValue("console");
    if (console.get() && console->IsObject()) {
      CefRefPtr<CefV8Value> log = console->GetValue("log");
      if (log.get() && log->IsFunction()) {
        CefV8ValueList largs;
        largs.push_back(CefV8Value::CreateString("[iwerBridge] applied pose (cef)"));
        log->ExecuteFunctionWithContext(context, console, largs);
      }
    }
  }
  // Also send a browser-process message for guaranteed visibility in stdout.
  CefRefPtr<CefFrame> frame = context->GetFrame();
  if (frame.get()) {
    CefRefPtr<CefProcessMessage> pm = CefProcessMessage::Create("RB_LOG");
    auto msgArgs = pm->GetArgumentList();
    msgArgs->SetString(0, "iwerBridge applied pose");
    frame->SendProcessMessage(PID_BROWSER, pm);

    CefRefPtr<CefProcessMessage> pm2 = CefProcessMessage::Create("RB_POSE");
    auto poseArgs = pm2->GetArgumentList();
    poseArgs->SetDouble(0, hmd_pos[0]);
    poseArgs->SetDouble(1, hmd_pos[1]);
    poseArgs->SetDouble(2, hmd_pos[2]);
    frame->SendProcessMessage(PID_BROWSER, pm2);
  }
  return true;
}
