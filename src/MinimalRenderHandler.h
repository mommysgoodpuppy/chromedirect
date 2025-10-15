#pragma once

#include <include/cef_render_process_handler.h>
#include <include/cef_v8.h>
#include <chrono>
#include <cstdint>

// Minimal render-process handler that provides:
// - V8 extension for stage5 (setDevice/clearDevice) for WebXR emulation
// - Receives VR_STATE messages from browser process and applies pose to bound device
class MinimalRenderHandler : public CefRenderProcessHandler
{
public:
  MinimalRenderHandler();

  // CefRenderProcessHandler overrides
  void OnWebKitInitialized() override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // Public API for V8 extension callbacks
  CefRefPtr<CefV8Value> CreatePoseValue() const;
  bool HasPose() const { return has_pose_; }
  bool BindDevice(CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Value> device);
  void ClearDevice();
  bool ApplyPoseToDevice();

  IMPLEMENT_REFCOUNTING(MinimalRenderHandler);

private:
  struct ControllerBinding
  {
    CefRefPtr<CefV8Value> value;
    CefRefPtr<CefV8Value> position;
    CefRefPtr<CefV8Value> position_set;
    CefRefPtr<CefV8Value> quaternion;
    CefRefPtr<CefV8Value> quaternion_set;
  };

  struct PoseSample
  {
    double hmd_pos[3] = {0};
    double hmd_quat[4] = {0, 0, 0, 1};
    double left_pos[3] = {0};
    double left_quat[4] = {0, 0, 0, 1};
    double right_pos[3] = {0};
    double right_quat[4] = {0, 0, 0, 1};
    uint64_t sequence = 0;
  };

  void LogPoseApply(bool success, const std::string &reason);

  PoseSample last_pose_{};
  bool has_pose_ = false;
  uint64_t pose_sequence_ = 0;
  CefRefPtr<CefV8Context> device_context_;
  CefRefPtr<CefV8Value> device_value_;
  CefRefPtr<CefV8Value> device_position_;
  CefRefPtr<CefV8Value> device_position_set_;
  CefRefPtr<CefV8Value> device_quaternion_;
  CefRefPtr<CefV8Value> device_quaternion_set_;
  ControllerBinding left_binding_;
  ControllerBinding right_binding_;
  bool applying_pose_ = false;
  std::chrono::steady_clock::time_point first_apply_time_{};
  std::chrono::steady_clock::time_point last_log_time_{};
  uint64_t device_apply_count_ = 0;
  uint64_t device_apply_fail_ = 0;
};
