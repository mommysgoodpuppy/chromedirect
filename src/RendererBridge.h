#pragma once

#include <include/cef_app.h>
#include <include/cef_v8.h>
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

 private:
  void InstallBridgeScript();
  bool ApplyPoseToPage(CefRefPtr<CefV8Context> context,
                       const float* hmd_pos,
                       const float* hmd_quat,
                       const float* left_pos,
                       const float* left_quat,
                       const float* right_pos,
                       const float* right_quat);

  IMPLEMENT_REFCOUNTING(RendererBridge);
};

