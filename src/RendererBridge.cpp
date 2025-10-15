#include "RendererBridge.h"

#include <include/cef_app.h>
#include <include/cef_v8.h>
#include <include/cef_command_line.h>
#include <include/wrapper/cef_helpers.h>
#include <cstring>
#include <iostream>

namespace
{
  const char *kBridgeJS = R"JS(
(function(){
  var g = (typeof window !== 'undefined') ? window : this;
  native function __iwerGetPose();
  native function __iwerHasPose();
  if (!g.iwerBridge) g.iwerBridge = {};
  if (typeof g.iwerBridge.getPose !== 'function') {
    g.iwerBridge.getPose = function(){ return __iwerGetPose(); };
  }
  if (typeof g.iwerBridge.hasPose !== 'function') {
    g.iwerBridge.hasPose = function(){ return __iwerHasPose(); };
  }
})();
)JS";

  class PoseV8Handler : public CefV8Handler
  {
  public:
    explicit PoseV8Handler(CefRefPtr<RendererBridge> owner) : owner_(owner) {}

    bool Execute(const CefString &name,
                 CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList & /*arguments*/,
                 CefRefPtr<CefV8Value> &retval,
                 CefString & /*exception*/) override
    {
      CefRefPtr<RendererBridge> owner = owner_;
      if (!owner.get())
        return false;
      if (name == "__iwerGetPose")
      {
        retval = owner->HasPose() ? owner->CreatePoseValue()
                                  : CefV8Value::CreateNull();
        return true;
      }
      if (name == "__iwerHasPose")
      {
        retval = CefV8Value::CreateBool(owner->HasPose());
        return true;
      }
      return false;
    }

  private:
    CefRefPtr<RendererBridge> owner_;

    IMPLEMENT_REFCOUNTING(PoseV8Handler);
  };
} // namespace

void RendererBridge::OnWebKitInitialized()
{
  // Renderer process: JS bridge registration
  std::cout << "[RendererBridge] OnWebKitInitialized: registering bridge" << std::endl;
  InstallBridgeScript();
}

void RendererBridge::InstallBridgeScript()
{
  CefRegisterExtension("v8/iwer_pose", kBridgeJS, new PoseV8Handler(this));
}

void RendererBridge::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefV8Context> context)
{
  // Ensure iwerBridge exists even if the extension injection missed
  CEF_REQUIRE_RENDERER_THREAD();
  if (!context.get())
    return;
  if (!context->Enter())
    return;
  CefRefPtr<CefV8Value> global = context->GetGlobal();
  CefRefPtr<CefV8Value> existing = global->GetValue("iwerBridge");
  if (!existing.get() || !existing->IsObject())
  {
    const char *kFallback = R"( (function(){
      var w = (typeof window !== 'undefined') ? window : this;
      if (!w.iwerBridge) w.iwerBridge = {};
      if (typeof w.iwerBridge.getPose !== 'function') {
        w.iwerBridge.getPose = function(){ return null; };
      }
      if (typeof w.iwerBridge.hasPose !== 'function') {
        w.iwerBridge.hasPose = function(){ return false; };
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
                                              CefRefPtr<CefProcessMessage> message)
{
  // Runs on renderer process main thread.
  const CefString &name = message->GetName();
  if (name != "VR_STATE")
    return false;
  std::cout << "[RendererBridge] VR_STATE received" << std::endl;

  auto args = message->GetArgumentList();
  if (!args.get() || args->GetSize() < 1 || args->GetType(0) != VTYPE_BINARY)
    return false;
  CefRefPtr<CefBinaryValue> bin = args->GetBinary(0);
  size_t len = bin->GetSize();
  if (len < sizeof(float) * (3 + 4) * 3)
    return false;
  std::vector<char> buf(len);
  bin->GetData(buf.data(), len, 0);
  const float *pf = reinterpret_cast<const float *>(buf.data());

  const float *hmd_pos = pf + 0;     // 3
  const float *hmd_quat = pf + 3;    // 4
  const float *left_pos = pf + 7;    // 3
  const float *left_quat = pf + 10;  // 4
  const float *right_pos = pf + 14;  // 3
  const float *right_quat = pf + 17; // 4

  pose_sequence_++;
  last_pose_.sequence = pose_sequence_;
  for (int i = 0; i < 3; ++i)
  {
    last_pose_.hmd_pos[i] = hmd_pos[i];
    last_pose_.left_pos[i] = left_pos[i];
    last_pose_.right_pos[i] = right_pos[i];
  }
  for (int i = 0; i < 4; ++i)
  {
    last_pose_.hmd_quat[i] = hmd_quat[i];
    last_pose_.left_quat[i] = left_quat[i];
    last_pose_.right_quat[i] = right_quat[i];
  }
  has_pose_ = true;

  if (pose_sequence_ == 1 || (pose_sequence_ % 120) == 0)
  {
    std::cout << "[RendererBridge] Pose updated seq=" << pose_sequence_
              << " hmd.pos=(" << last_pose_.hmd_pos[0] << ", "
              << last_pose_.hmd_pos[1] << ", " << last_pose_.hmd_pos[2]
              << ")" << std::endl;
  }

  return true;
}

CefRefPtr<CefV8Value> RendererBridge::CreatePoseValue() const
{
  if (!has_pose_)
  {
    return CefV8Value::CreateNull();
  }

  auto makeVec = [](const double *p3)
  {
    CefRefPtr<CefV8Value> arr = CefV8Value::CreateArray(3);
    arr->SetValue(0, CefV8Value::CreateDouble(p3[0]));
    arr->SetValue(1, CefV8Value::CreateDouble(p3[1]));
    arr->SetValue(2, CefV8Value::CreateDouble(p3[2]));
    return arr;
  };
  auto makeQuat = [](const double *q4)
  {
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
    hmd->SetValue("pos", makeVec(last_pose_.hmd_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    hmd->SetValue("quat", makeQuat(last_pose_.hmd_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("hmd", hmd, V8_PROPERTY_ATTRIBUTE_NONE);
  }
  {
    CefRefPtr<CefV8Value> left = CefV8Value::CreateObject(nullptr, nullptr);
    left->SetValue("pos", makeVec(last_pose_.left_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    left->SetValue("quat", makeQuat(last_pose_.left_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("left", left, V8_PROPERTY_ATTRIBUTE_NONE);
  }
  {
    CefRefPtr<CefV8Value> right = CefV8Value::CreateObject(nullptr, nullptr);
    right->SetValue("pos", makeVec(last_pose_.right_pos), V8_PROPERTY_ATTRIBUTE_NONE);
    right->SetValue("quat", makeQuat(last_pose_.right_quat), V8_PROPERTY_ATTRIBUTE_NONE);
    state->SetValue("right", right, V8_PROPERTY_ATTRIBUTE_NONE);
  }

  state->SetValue("sequence",
                  CefV8Value::CreateDouble(static_cast<double>(last_pose_.sequence)),
                  V8_PROPERTY_ATTRIBUTE_NONE);
  return state;
}
