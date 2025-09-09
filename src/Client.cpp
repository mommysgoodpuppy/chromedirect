#include "Client.h"

#include <include/cef_browser.h>
#include <include/cef_command_line.h>
#include <include/cef_origin_whitelist.h>
#include <include/cef_request.h>
#include <include/cef_sandbox_win.h>
#include <include/wrapper/cef_helpers.h>

#include <string>
#include <iostream>
#include <algorithm>
#include <cstring>

OffscreenClient::OffscreenClient(HWND host_window, std::shared_ptr<Presenter> presenter, int width, int height, float scale, int frame_rate)
    : host_window_(host_window), presenter_(std::move(presenter)), width_(width), height_(height), scale_(scale), frame_rate_(frame_rate) {
  std::cout << "[Client] OffscreenClient created (" << width << "x" << height << ", scale=" << scale << ")\n";
}

void OffscreenClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_ = browser;
  std::cout << "[Client] Browser created successfully! ID: " << browser->GetIdentifier() << "\n";
  if (frame_rate_ > 0) {
    browser->GetHost()->SetWindowlessFrameRate(frame_rate_);
    std::cout << "[Client] Windowless frame rate set to " << frame_rate_ << " FPS\n";
  }
  
  // Force an invalidation to trigger paint events
  browser->GetHost()->Invalidate(PET_VIEW);
  std::cout << "[Client] Forced browser invalidation to trigger paint\n";
}

void OffscreenClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Client] Browser closing... ID: " << browser->GetIdentifier() << "\n";
  browser_ = nullptr;
  // Signal that the browser is closed - this helps with shutdown
  if (host_window_) {
    PostMessage(host_window_, WM_DESTROY, 0, 0);
  }
  std::cout << "[Client] Browser closed\n";
}

void OffscreenClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
  rect = CefRect(0, 0, width_, height_);
  static bool logged = false;
  if (!logged) {
    std::cout << "[Client] GetViewRect called: " << width_ << "x" << height_ << "\n";
    logged = true;
  }
}

bool OffscreenClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
  screen_info.device_scale_factor = scale_;
  screen_info.depth = 24;
  screen_info.depth_per_component = 8;
  screen_info.is_monochrome = false;
  screen_info.rect = CefRect(0, 0, width_, height_);
  screen_info.available_rect = screen_info.rect;
  static bool logged = false;
  if (!logged) {
    std::cout << "[Client] GetScreenInfo called: scale=" << scale_ << ", depth=24\n";
    logged = true;
  }
  return true;
}

void OffscreenClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                                         PaintElementType type,
                                         const RectList& dirty_rects,
                                         const CefAcceleratedPaintInfo& info) {
  CEF_REQUIRE_UI_THREAD();
  if (type != PET_VIEW) return;
  
  static int paint_count = 0;
  paint_count++;
  
  if (paint_count <= 10 || paint_count % 60 == 0) {
    std::cout << "[Client] OnAcceleratedPaint #" << paint_count << " - Handle: " << info.shared_texture_handle 
              << ", Size: " << width_ << "x" << height_ << ", Format: " << info.format << "\n";
    std::cout << "[Client] Dirty rects count: " << dirty_rects.size() << "\n";
  }
  
  got_accel_.store(true, std::memory_order_relaxed);
  if (presenter_) {
    presenter_->PresentSharedHandle(info.shared_texture_handle, width_, height_);
  } else {
    std::cerr << "[Client] ERROR: No presenter available for OnAcceleratedPaint!\n";
  }
}

void OffscreenClient::OnPaint(CefRefPtr<CefBrowser> browser,
                              PaintElementType type,
                              const RectList& dirty_rects,
                              const void* buffer,
                              int width,
                              int height) {
  // Fallback path when GPU is disabled; not implemented for brevity.
  static int software_paint_count = 0;
  software_paint_count++;
  std::cout << "[Client] OnPaint (software) #" << software_paint_count << " - Size: " << width << "x" << height << "\n";
  std::cout << "[Client] WARNING: Software rendering fallback - GPU acceleration may not be working!\n";
}

void OffscreenClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Client] Title changed: " << title.ToString() << "\n";
  if (host_window_) SetWindowTextW(host_window_, std::wstring(title).c_str());
}

bool OffscreenClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                       cef_log_severity_t level,
                                       const CefString& message,
                                       const CefString& source,
                                       int line) {
  CEF_REQUIRE_UI_THREAD();
  const char* sev = "VERBOSE";
  switch (level) {
    case LOGSEVERITY_DEBUG: sev = "DEBUG"; break;
    case LOGSEVERITY_INFO: sev = "INFO"; break;
    case LOGSEVERITY_WARNING: sev = "WARN"; break;
    case LOGSEVERITY_ERROR: sev = "ERROR"; break;
    case LOGSEVERITY_FATAL: sev = "FATAL"; break;
    default: break;
  }
  std::cout << "[Console][" << sev << "] " << message.ToString()
            << " (" << source.ToString() << ":" << line << ")\n";
  // return false to allow default handling as well; true would suppress it
  return false;
}

bool OffscreenClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request,
                                     bool user_gesture,
                                     bool is_redirect) {
  return false;  // allow
}

 

/* void OffscreenClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                           bool isLoading,
                                           bool canGoBack,
                                           bool canGoForward) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Load] state change: isLoading=" << (isLoading?"1":"0")
            << " canGoBack=" << (canGoBack?"1":"0")
            << " canGoForward=" << (canGoForward?"1":"0") << "\n";
} */

// (Load and response diagnostics removed)

bool OffscreenClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               CefProcessId source_process,
                                               CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();
  // For now we don't handle any renderer->browser messages in this build.
  // This override exists to satisfy the declaration and allow future use.
  return false;
}

 
