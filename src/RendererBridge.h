#pragma once

#include <include/cef_app.h>
#include <include/cef_v8.h>
#include <cstdint>
#include <string>

// RendererBridge handles render-process side: injects a small JS bridge and
// receives VR state messages from the browser process, applying them to IWER.
class RendererBridge : public CefRenderProcessHandler {
 public:
  RendererBridge() = default;

  void OnWebKitInitialized() override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

 public:
  void InstallBridgeScript();

  struct PoseSample {
    double hmd_pos[3] = {0};
    double hmd_quat[4] = {0, 0, 0, 1};
    double left_pos[3] = {0};
    double left_quat[4] = {0, 0, 0, 1};
    double right_pos[3] = {0};
    double right_quat[4] = {0, 0, 0, 1};
    uint64_t sequence = 0;
  };

  CefRefPtr<CefV8Value> CreatePoseValue() const;
  bool HasPose() const { return has_pose_; }

  PoseSample last_pose_{};
  bool has_pose_ = false;
  uint64_t pose_sequence_ = 0;

  IMPLEMENT_REFCOUNTING(RendererBridge);
};
