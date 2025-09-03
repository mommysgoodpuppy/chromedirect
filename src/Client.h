#pragma once

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_display_handler.h>
#include <include/cef_request_handler.h>
#include <windows.h>
#include <memory>
#include <atomic>

class OpenVRPresenter;
class D3DPresenter;

class OffscreenClient final : public CefClient,
                              public CefLifeSpanHandler,
                              public CefRenderHandler,
                              public CefDisplayHandler,
                              public CefRequestHandler {
public:
  OffscreenClient(HWND host_window, std::shared_ptr<OpenVRPresenter> presenter, int width, int height, float scale = 1.0f);
  OffscreenClient(HWND host_window, std::shared_ptr<D3DPresenter> presenter, int width, int height, float scale = 1.0f);

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

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

  // CefRequestHandler
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;

  HWND GetHostHwnd() const { return host_window_; }
  CefRefPtr<CefBrowser> GetBrowser() const { return browser_; }

private:
  HWND host_window_ = nullptr;
  std::shared_ptr<void> presenter_; // Can hold either OpenVRPresenter or D3DPresenter
  bool is_vr_mode_;
  CefRefPtr<CefBrowser> browser_;
  int width_ = 800;
  int height_ = 600;
  float scale_ = 1.0f;
  std::atomic<bool> got_accel_{false};

  IMPLEMENT_REFCOUNTING(OffscreenClient);
  DISALLOW_COPY_AND_ASSIGN(OffscreenClient);
};

