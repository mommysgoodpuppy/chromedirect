#pragma once

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_display_handler.h>
#include <include/cef_request_handler.h>
#include <include/cef_load_handler.h>
#include <windows.h>
#include <memory>
#include <atomic>
#include "Presenter.h"

class OffscreenClient final : public CefClient,
                              public CefLifeSpanHandler,
                              public CefRenderHandler,
                              public CefDisplayHandler,
                              public CefRequestHandler,
                              public CefLoadHandler {
public:
  OffscreenClient(HWND host_window,
                  std::shared_ptr<Presenter> presenter,
                  int width,
                  int height,
                  float scale = 1.0f,
                  int frame_rate = 60,
                  bool vr_mode = false);

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;
  

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefRenderHandler
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
  void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                          PaintElementType type,
                          const RectList& dirty_rects,
                          const CefAcceleratedPaintInfo& info) override;
  void OnPaint(CefRefPtr<CefBrowser> browser,
               PaintElementType type,
               const RectList& dirty_rects,
               const void* buffer,
               int width,
               int height) override;

  // CefDisplayHandler
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override;

  // CefRequestHandler
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;

  // CefLoadHandler
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;

  HWND GetHostHwnd() const { return host_window_; }
  CefRefPtr<CefBrowser> GetBrowser() const { return browser_; }

private:
  HWND host_window_ = nullptr;
  std::shared_ptr<Presenter> presenter_;
  CefRefPtr<CefBrowser> browser_;
  int width_ = 800;
  int height_ = 600;
  float scale_ = 1.0f;
  int frame_rate_ = 60;
  bool vr_mode_ = false;
  std::atomic<bool> got_accel_{false};

  // Minimal V8 ping (debug) to validate page script execution without renderer handler
  bool v8_ping_enabled_ = false;
  int v8_ping_ms_ = 0;

  // Minimal VR postMessage (debug) to validate a browser->page data path
  bool v8_vr_enabled_ = false;
  int v8_vr_ms_ = 0;
  int v8_vr_tick_ = 0;

  bool SendPoseToRenderer(CefRefPtr<CefFrame> frame,
                          const float* hmd_pos,
                          const float* hmd_quat,
                          const float* left_pos,
                          const float* left_quat,
                          const float* right_pos,
                          const float* right_quat);

  IMPLEMENT_REFCOUNTING(OffscreenClient);
  DISALLOW_COPY_AND_ASSIGN(OffscreenClient);
};
