#include "Client.h"

#include <include/cef_browser.h>
#include <include/cef_command_line.h>
#include <include/cef_origin_whitelist.h>
#include <include/cef_request.h>
#include <include/cef_sandbox_win.h>
#include <include/wrapper/cef_helpers.h>
#include <include/cef_task.h>

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

  // Optional: minimal V8 ping to validate page JS runs without render handler
  CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
  if (cmd.get() && cmd->HasSwitch("v8-ping")) {
    int ms = std::max(100, atoi(cmd->GetSwitchValue("v8-ping").ToString().c_str()));
    v8_ping_enabled_ = true;
    v8_ping_ms_ = ms;
    std::cout << "[Client] V8 ping enabled (interval=" << v8_ping_ms_ << " ms)\n";
    class PingTask : public CefTask {
     public:
      PingTask(CefRefPtr<OffscreenClient> c, int ms) : client_(c), ms_(ms) {}
      void Execute() override {
        CEF_REQUIRE_UI_THREAD();
        if (!client_.get()) return;
        auto br = client_->GetBrowser();
        if (!br.get()) return;
        auto frame = br->GetMainFrame();
        if (frame.get()) {
          frame->ExecuteJavaScript(R"(window.__cefPingCount=(window.__cefPingCount||0)+1; console.log('[cef-ping]', window.__cefPingCount);)", "", 0);
        }
        // Re-schedule if still enabled
        CefPostDelayedTask(TID_UI, new PingTask(client_, ms_), ms_);
      }
     private:
      CefRefPtr<OffscreenClient> client_;
      int ms_;
      IMPLEMENT_REFCOUNTING(PingTask);
    };
    CefPostDelayedTask(TID_UI, new PingTask(this, v8_ping_ms_), v8_ping_ms_);
  }

  // Optional: minimal VR data via postMessage to page
  if (cmd.get() && cmd->HasSwitch("v8-post-vr")) {
    int ms = std::max(100, atoi(cmd->GetSwitchValue("v8-post-vr").ToString().c_str()));
    v8_vr_enabled_ = true;
    v8_vr_ms_ = ms;
    v8_vr_tick_ = 0;
    std::cout << "[Client] V8 VR postMessage enabled (interval=" << v8_vr_ms_ << " ms)\n";

    // Install a simple page-side listener (idempotent) to log incoming messages
    auto frame = browser->GetMainFrame();
    if (frame.get()) {
      frame->ExecuteJavaScript(R"JS((function(){
        try {
          if (!window.__cefVrListener) {
            window.addEventListener('message', function(ev){
              try {
                if (ev && ev.data && ev.data.type === 'cef-vr') {
                  var s = ev.data.state || {};
                  var p = s.hmd && s.hmd.pos; var q = s.hmd && s.hmd.quat;
                  console.log('[cef-vr]', p, q);
                }
              } catch(e) {}
            });
            window.__cefVrListener = true;
          }
        } catch(e) {}
      })();)JS", "", 0);
    }

    // Schedule postMessage sender
    class VrTask : public CefTask {
     public:
      explicit VrTask(CefRefPtr<OffscreenClient> c) : client_(c) {}
      void Execute() override {
        CEF_REQUIRE_UI_THREAD();
        if (!client_.get()) return;
        auto br = client_->GetBrowser();
        if (!br.get()) return;
        auto frame = br->GetMainFrame();
        if (!frame.get()) return;
        // Dummy animated pose: small circle, tick-based
        client_->v8_vr_tick_++;
        double t = client_->v8_vr_tick_ * (client_->v8_vr_ms_ / 1000.0);
        double r = 0.25;
        double x = r * sin(t), y = 1.6, z = r * cos(t);
        char js[512];
        snprintf(js, sizeof(js),
          "(function(){ try{ window.postMessage({type:'cef-vr', state:{hmd:{pos:[%f,%f,%f], quat:[0,0,0,1]}}}, '*'); }catch(e){} })();",
          x, y, z);
        frame->ExecuteJavaScript(js, "", 0);
        // Re-schedule
        CefPostDelayedTask(TID_UI, new VrTask(client_), client_->v8_vr_ms_);
      }
     private:
      CefRefPtr<OffscreenClient> client_;
      IMPLEMENT_REFCOUNTING(VrTask);
    };
    CefPostDelayedTask(TID_UI, new VrTask(this), v8_vr_ms_);
  }
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

 
