#include "MinimalRenderHandler.h"
#include <include/cef_browser.h>
#include <include/wrapper/cef_helpers.h>
#include <iostream>
#include <vector>

MinimalRenderHandler::MinimalRenderHandler() {}

void MinimalRenderHandler::OnWebKitInitialized()
{
  // Register the V8 extension that provides cefExt.webxr.setDevice/clearDevice
  class WebXRExtensionHandler : public CefV8Handler
  {
  public:
    explicit WebXRExtensionHandler(MinimalRenderHandler *owner) : owner_(owner) {}
    bool Execute(const CefString &name,
                 CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList &arguments,
                 CefRefPtr<CefV8Value> &retval,
                 CefString & /*exception*/) override
    {
      if (!owner_)
        return false;
      if (name == "__setDevice")
      {
        CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
        bool ok = false;
        if (arguments.size() > 0 && arguments[0].get() && arguments[0]->IsObject())
        {
          ok = owner_->BindDevice(ctx, arguments[0]);
        }
        retval = CefV8Value::CreateBool(ok);
        return true;
      }
      if (name == "__clearDevice")
      {
        owner_->ClearDevice();
        retval = CefV8Value::CreateBool(true);
        return true;
      }
      return false;
    }

  private:
    MinimalRenderHandler *owner_;
    IMPLEMENT_REFCOUNTING(WebXRExtensionHandler);
  };

  const char *kWebXRExtensionJS =
      "if (!cefExt) var cefExt = {};\n"
      "if (!cefExt.webxr) cefExt.webxr = {};\n"
      "native function __setDevice();\n"
      "native function __clearDevice();\n"
      "cefExt.webxr.setDevice = function(dev){ return __setDevice(dev); };\n"
      "cefExt.webxr.clearDevice = function(){ return __clearDevice(); };\n";
  CefRegisterExtension("v8/cef_webxr", kWebXRExtensionJS, new WebXRExtensionHandler(this));
}

void MinimalRenderHandler::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefRefPtr<CefV8Context> context)
{
  // Post a simple log to the browser process
  if (!frame.get())
    return;

  CefRefPtr<CefProcessMessage> pm0 = CefProcessMessage::Create("RB_LOG");
  pm0->GetArgumentList()->SetString(0, "MinimalRenderHandler: OnContextCreated");
  frame->SendProcessMessage(PID_BROWSER, pm0);

  CefRefPtr<CefProcessMessage> pm = CefProcessMessage::Create("RB_LOG");
  std::string info = std::string("MinimalRenderHandler info: main=") + (frame->IsMain() ? "1" : "0") +
                     " has_pose=" + (has_pose_ ? "1" : "0");
  pm->GetArgumentList()->SetString(0, info);
  frame->SendProcessMessage(PID_BROWSER, pm);
}

void MinimalRenderHandler::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                             CefRefPtr<CefFrame> frame,
                                             CefRefPtr<CefV8Context> context)
{
  if (device_context_.get() && context.get() && context->IsSame(device_context_))
  {
    ClearDevice();
  }
}

bool MinimalRenderHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                                    CefRefPtr<CefFrame> frame,
                                                    CefProcessId source_process,
                                                    CefRefPtr<CefProcessMessage> message)
{
  if (!message.get())
    return false;
  if (message->GetName() != "VR_STATE")
    return false;

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

  const float *hmd_pos = pf + 0;
  const float *hmd_quat = pf + 3;
  const float *left_pos = pf + 7;
  const float *left_quat = pf + 10;
  const float *right_pos = pf + 14;
  const float *right_quat = pf + 17;

  pose_sequence_++;
  for (int i = 0; i < 3; ++i)
  {
    last_pose_.hmd_pos[i] = static_cast<double>(hmd_pos[i]);
    last_pose_.left_pos[i] = static_cast<double>(left_pos[i]);
    last_pose_.right_pos[i] = static_cast<double>(right_pos[i]);
  }
  for (int i = 0; i < 4; ++i)
  {
    last_pose_.hmd_quat[i] = static_cast<double>(hmd_quat[i]);
    last_pose_.left_quat[i] = static_cast<double>(left_quat[i]);
    last_pose_.right_quat[i] = static_cast<double>(right_quat[i]);
  }
  last_pose_.sequence = pose_sequence_;
  has_pose_ = true;

  ApplyPoseToDevice();

  if (pose_sequence_ == 1 || (pose_sequence_ % 120) == 0)
  {
    std::cout << "[MinimalRenderHandler] Pose updated seq=" << pose_sequence_
              << " hmd.pos=(" << last_pose_.hmd_pos[0] << ", "
              << last_pose_.hmd_pos[1] << ", " << last_pose_.hmd_pos[2]
              << ")" << std::endl;
  }
  return true;
}

CefRefPtr<CefV8Value> MinimalRenderHandler::CreatePoseValue() const
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

bool MinimalRenderHandler::BindDevice(CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Value> device)
{
  if (!context.get() || !device.get() || !device->IsObject())
  {
    std::cout << "[MinimalRenderHandler] WARN webxr.setDevice invalid arguments" << std::endl;
    return false;
  }

  ClearDevice();
  device_context_ = context;
  device_value_ = device;

  auto getObject = [](CefRefPtr<CefV8Value> obj, const char *name) -> CefRefPtr<CefV8Value>
  {
    if (!obj.get())
      return nullptr;
    CefRefPtr<CefV8Value> value = obj->GetValue(name);
    if (value.get() && value->IsObject())
      return value;
    return nullptr;
  };
  auto getFunction = [](CefRefPtr<CefV8Value> obj, const char *name) -> CefRefPtr<CefV8Value>
  {
    if (!obj.get())
      return nullptr;
    CefRefPtr<CefV8Value> value = obj->GetValue(name);
    if (value.get() && value->IsFunction())
      return value;
    return nullptr;
  };

  device_position_ = getObject(device, "position");
  device_position_set_ = getFunction(device_position_, "set");
  device_quaternion_ = getObject(device, "quaternion");
  device_quaternion_set_ = getFunction(device_quaternion_, "set");

  CefRefPtr<CefV8Value> controllers = getObject(device, "controllers");
  if (controllers.get())
  {
    CefRefPtr<CefV8Value> left = getObject(controllers, "left");
    if (left.get())
    {
      left_binding_.value = left;
      left_binding_.position = getObject(left, "position");
      left_binding_.position_set = getFunction(left_binding_.position, "set");
      left_binding_.quaternion = getObject(left, "quaternion");
      left_binding_.quaternion_set = getFunction(left_binding_.quaternion, "set");
    }
    CefRefPtr<CefV8Value> right = getObject(controllers, "right");
    if (right.get())
    {
      right_binding_.value = right;
      right_binding_.position = getObject(right, "position");
      right_binding_.position_set = getFunction(right_binding_.position, "set");
      right_binding_.quaternion = getObject(right, "quaternion");
      right_binding_.quaternion_set = getFunction(right_binding_.quaternion, "set");
    }
  }

  const bool ok = device_position_set_.get() && device_quaternion_set_.get();
  if (!ok)
  {
    std::cout << "[MinimalRenderHandler] WARN webxr.setDevice missing position/quaternion setters" << std::endl;
  }
  else
  {
    std::cout << "[MinimalRenderHandler] webxr.setDevice bound" << std::endl;
  }

  if (ok && has_pose_)
  {
    ApplyPoseToDevice();
  }
  return ok;
}

void MinimalRenderHandler::ClearDevice()
{
  if (device_context_.get())
  {
    std::cout << "[MinimalRenderHandler] webxr.clearDevice" << std::endl;
  }
  device_context_ = nullptr;
  device_value_ = nullptr;
  device_position_ = nullptr;
  device_position_set_ = nullptr;
  device_quaternion_ = nullptr;
  device_quaternion_set_ = nullptr;
  left_binding_ = ControllerBinding();
  right_binding_ = ControllerBinding();
  applying_pose_ = false;
}

bool MinimalRenderHandler::ApplyPoseToDevice()
{
  if (!device_context_.get())
    return false;
  if (!device_position_set_.get() || !device_quaternion_set_.get())
    return false;
  if (applying_pose_)
    return true;

  bool entered = device_context_->Enter();
  if (!entered)
    return false;

  applying_pose_ = true;
  auto callVec3 = [](CefRefPtr<CefV8Value> target,
                     CefRefPtr<CefV8Value> fn,
                     const double *v)
  {
    if (!target.get() || !fn.get() || !fn->IsFunction())
      return false;
    CefV8ValueList args;
    args.push_back(CefV8Value::CreateDouble(v[0]));
    args.push_back(CefV8Value::CreateDouble(v[1]));
    args.push_back(CefV8Value::CreateDouble(v[2]));
    fn->ExecuteFunction(target, args);
    return true;
  };
  auto callQuat = [](CefRefPtr<CefV8Value> target,
                     CefRefPtr<CefV8Value> fn,
                     const double *v)
  {
    if (!target.get() || !fn.get() || !fn->IsFunction())
      return false;
    CefV8ValueList args;
    args.push_back(CefV8Value::CreateDouble(v[0]));
    args.push_back(CefV8Value::CreateDouble(v[1]));
    args.push_back(CefV8Value::CreateDouble(v[2]));
    args.push_back(CefV8Value::CreateDouble(v[3]));
    fn->ExecuteFunction(target, args);
    return true;
  };

  bool ok_hmd_pos = callVec3(device_position_, device_position_set_, last_pose_.hmd_pos);
  bool ok_hmd_quat = callQuat(device_quaternion_, device_quaternion_set_, last_pose_.hmd_quat);

  auto applyController = [&](ControllerBinding &binding,
                             const double *pos,
                             const double *quat) -> bool
  {
    if (!binding.value.get())
      return true;
    bool ok = true;
    if (binding.position.get() || binding.position_set.get())
    {
      ok &= callVec3(binding.position, binding.position_set, pos);
    }
    if (binding.quaternion.get() || binding.quaternion_set.get())
    {
      ok &= callQuat(binding.quaternion, binding.quaternion_set, quat);
    }
    return ok;
  };
  bool ok_left = applyController(left_binding_, last_pose_.left_pos, last_pose_.left_quat);
  bool ok_right = applyController(right_binding_, last_pose_.right_pos, last_pose_.right_quat);

  applying_pose_ = false;
  device_context_->Exit();

  bool success = ok_hmd_pos && ok_hmd_quat && ok_left && ok_right;
  std::string reason;
  if (!success)
  {
    if (!ok_hmd_pos)
      reason = "device.position.set";
    else if (!ok_hmd_quat)
      reason = "device.quaternion.set";
    else if (!ok_left)
      reason = "left controller setters";
    else
      reason = "right controller setters";
  }
  LogPoseApply(success, reason);
  return success;
}

void MinimalRenderHandler::LogPoseApply(bool success, const std::string &reason)
{
  auto now = std::chrono::steady_clock::now();
  if (first_apply_time_ == std::chrono::steady_clock::time_point())
  {
    first_apply_time_ = now;
  }
  device_apply_count_++;
  if (!success)
  {
    device_apply_fail_++;
    std::cout << "[MinimalRenderHandler] webxr.applyPose FAILED count=" << device_apply_count_
              << " fails=" << device_apply_fail_ << " reason=" << reason << std::endl;
    last_log_time_ = now;
    return;
  }

  if (device_apply_count_ == 1 || (device_apply_count_ % 120) == 0 ||
      (now - last_log_time_) >= std::chrono::seconds(5))
  {
    double elapsed_ms = std::chrono::duration<double, std::milli>(now - first_apply_time_).count();
    double hz = elapsed_ms > 0.0 ? (device_apply_count_ * 1000.0) / elapsed_ms : 0.0;
    std::cout << "[MinimalRenderHandler] webxr.applyPose OK count=" << device_apply_count_
              << " avg=" << hz << "Hz fails=" << device_apply_fail_ << std::endl;
    last_log_time_ = now;
  }
}
