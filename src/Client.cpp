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
  std::cout << "[Client] OnBeforeBrowse: " << request->GetURL().ToString()
            << ", main=" << (frame.get() && frame->IsMain() ? "1" : "0")
            << ", user_gesture=" << (user_gesture ? "1" : "0")
            << ", redirect=" << (is_redirect ? "1" : "0")
            << "\n";
  return false;  // allow
}

cef_return_value_t OffscreenClient::OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                                         CefRefPtr<CefFrame> frame,
                                                         CefRefPtr<CefRequest> request,
                                                         CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  const std::string url = request->GetURL();
  // Heuristic: cancel common live-reload scripts and websocket endpoints that can trigger reloads.
  if (url.find("livereload") != std::string::npos ||
      url.find("live-server") != std::string::npos ||
      url.find("/ws") != std::string::npos && url.find(":5501") != std::string::npos) {
    std::cout << "[Client] Blocking resource: " << url << "\n";
    return RV_CANCEL;
  }
  return RV_CONTINUE;
}

void OffscreenClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                           bool isLoading,
                                           bool canGoBack,
                                           bool canGoForward) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Load] state change: isLoading=" << (isLoading?"1":"0")
            << " canGoBack=" << (canGoBack?"1":"0")
            << " canGoForward=" << (canGoForward?"1":"0") << "\n";
}

void OffscreenClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefLoadHandler::TransitionType transition_type) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Load] start url=" << frame->GetURL().ToString()
            << " main=" << (frame->IsMain()?"1":"0")
            << " transition=" << transition_type << "\n";
}

void OffscreenClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                int httpStatusCode) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Load] end url=" << frame->GetURL().ToString()
            << " main=" << (frame->IsMain()?"1":"0")
            << " code=" << httpStatusCode << "\n";
  // Install navigation diagnostics in the main frame
  if (frame->IsMain()) {
    frame->ExecuteJavaScript(R"JS((function(){
      try {
        // Title marker for reload diagnostics (works even if console is suppressed)
        window.__navCount = (window.__navCount||0)+1;
        var navType = 'unknown';
        try {
          var navEntry = (performance.getEntriesByType && performance.getEntriesByType('navigation')[0]) || null;
          if (navEntry && navEntry.type) navType = navEntry.type;
          else if (performance.navigation) {
            navType = (performance.navigation.type===1)?'reload':(performance.navigation.type===2?'back_forward':'navigate');
          }
        } catch(e){}
        try {
          var base = document.title.replace(/ \[nr=.*\]$/,'');
          document.title = base + ' [nr=' + window.__navCount + ',type=' + navType + ']';
        } catch(e){}

        // Optional console diagnostics for nav sources
        function wrap(obj, name){
          if (!obj) return; var orig=obj[name];
          if (typeof orig==='function'){
            obj[name]=function(){ try{ console.log('[nav]', name); console.trace(); }catch(e){} return orig.apply(this, arguments); };
          }
        }
        wrap(location,'reload'); wrap(location,'assign'); wrap(location,'replace');
        if (history){
          if (history.pushState){ var o=history.pushState; history.pushState=function(){ try{ console.log('[nav] pushState'); console.trace(); }catch(e){} return o.apply(this, arguments);} }
          if (history.replaceState){ var o2=history.replaceState; history.replaceState=function(){ try{ console.log('[nav] replaceState'); console.trace(); }catch(e){} return o2.apply(this, arguments);} }
        }
        window.addEventListener('beforeunload', function(){ try{ console.log('[nav] beforeunload'); }catch(e){} });
        window.addEventListener('unload', function(){ try{ console.log('[nav] unload'); }catch(e){} });
        document.addEventListener('visibilitychange', function(){ try{ console.log('[nav] visibility', document.visibilityState); }catch(e){} });
      } catch(e) {}
    })();)JS", "", 0);
  }
}

void OffscreenClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  cef_errorcode_t errorCode,
                                  const CefString& errorText,
                                  const CefString& failedUrl) {
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Load] error url=" << failedUrl.ToString()
            << " main=" << (frame->IsMain()?"1":"0")
            << " code=" << errorCode << " text=" << errorText.ToString() << "\n";
}

void OffscreenClient::OnResourceLoadComplete(CefRefPtr<CefBrowser> browser,
                                             CefRefPtr<CefFrame> frame,
                                             CefRefPtr<CefRequest> request,
                                             CefRefPtr<CefResponse> response,
                                             CefResourceRequestHandler::URLRequestStatus status,
                                             int64_t received_content_length) {
  CEF_REQUIRE_IO_THREAD();
  std::cout << "[ResComplete] url=" << request->GetURL().ToString()
            << " status_code=" << response->GetStatus()
            << " req_status=" << status
            << " bytes=" << received_content_length
            << " main=" << ((frame.get() && frame->IsMain())?"1":"0")
            << "\n";
}

bool OffscreenClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               CefProcessId source_process,
                                               CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();
  // For now we don't handle any renderer->browser messages in this build.
  // This override exists to satisfy the declaration and allow future use.
  return false;
}

namespace {
class SniffFilter : public CefResponseFilter {
 public:
  explicit SniffFilter(const std::string& url, bool main_frame)
    : url_(url), main_frame_(main_frame) {}

  bool InitFilter() override { return true; }

  FilterStatus Filter(void* data_in,
                      size_t data_in_size,
                      size_t& data_in_read,
                      void* data_out,
                      size_t data_out_size,
                      size_t& data_out_written) override {
    data_in_read = 0;
    data_out_written = 0;

    // 1) Emit any pending bytes first
    if (!pending_out_.empty() && data_out && data_out_size > 0) {
      const size_t to_write = std::min(data_out_size, pending_out_.size());
      std::memcpy(data_out, pending_out_.data(), to_write);
      pending_out_.erase(0, to_write);
      data_out_written += to_write;
    }

    // 2) If we still have room in output, copy from input directly (pass-through)
    if (data_out && data_out_written < data_out_size && data_in && data_in_size > 0) {
      const size_t space = data_out_size - data_out_written;
      const size_t to_copy_now = std::min(space, data_in_size);
      if (to_copy_now > 0) {
        std::memcpy(static_cast<char*>(data_out) + data_out_written, data_in, to_copy_now);
        data_in_read += to_copy_now;
        data_out_written += to_copy_now;
      }
      // Buffer the remaining input (if any) to emit later
      if (data_in_size > to_copy_now) {
        pending_out_.append(static_cast<const char*>(data_in) + to_copy_now,
                            data_in_size - to_copy_now);
        data_in_read += (data_in_size - to_copy_now); // consume all input
      }

      // Sniff only up to a cap
      if (!logged_) {
        const size_t kMaxBuf = 128 * 1024;
        const size_t remain = (kMaxBuf > buffer_.size()) ? (kMaxBuf - buffer_.size()) : 0;
        const size_t sniff_copy = std::min(remain, data_in_size);
        if (sniff_copy > 0) buffer_.append(static_cast<const char*>(data_in), sniff_copy);

        std::string lower = buffer_;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (!logged_) {
          if (lower.find("http-equiv") != std::string::npos && lower.find("refresh") != std::string::npos) {
            std::cout << "[Sniff] META refresh-like markers in " << (main_frame_?"main":"sub")
                      << " frame: " << url_ << "\n";
            logged_ = true;
          } else if (lower.find("livereload") != std::string::npos || lower.find("live-server") != std::string::npos) {
            std::cout << "[Sniff] Live-reload marker detected in HTML: " << url_ << "\n";
            logged_ = true;
          }
        }
      }
    }

    if (!pending_out_.empty()) return RESPONSE_FILTER_NEED_MORE_DATA;
    return RESPONSE_FILTER_DONE;
  }

 private:
  std::string url_;
  bool main_frame_;
  std::string buffer_;
  std::string pending_out_;
  bool logged_ = false;
  IMPLEMENT_REFCOUNTING(SniffFilter);
};
} // namespace

CefRefPtr<CefResponseFilter> OffscreenClient::GetResourceResponseFilter(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();
  // Only sniff main-frame HTML to detect META refresh or injected reload markers
  const bool is_main = frame.get() && frame->IsMain();
  const std::string mime = response->GetMimeType().ToString();
  if (is_main && mime == "text/html") {
    return new SniffFilter(request->GetURL(), true);
  }
  return nullptr;
}
