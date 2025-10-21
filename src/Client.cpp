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
#include <cmath>
#include <cstdio>
#include <chrono>
#include <iomanip>
#include <openvr.h>

OffscreenClient::OffscreenClient(HWND host_window,
                                 std::shared_ptr<Presenter> presenter,
                                 int width,
                                 int height,
                                 float scale,
                                 int frame_rate,
                                 bool vr_mode)
    : host_window_(host_window),
      presenter_(std::move(presenter)),
      width_(width),
      height_(height),
      scale_(scale),
      frame_rate_(frame_rate),
      vr_mode_(vr_mode)
{
  std::cout << "[Client] OffscreenClient created (" << width << "x" << height
            << ", scale=" << scale << ", vr_mode=" << (vr_mode_ ? "1" : "0") << ")\n";
}

void OffscreenClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();
  browser_ = browser;
  std::cout << "[Client] Browser created successfully! ID: " << browser->GetIdentifier() << "\n";
  if (frame_rate_ > 0)
  {
    browser->GetHost()->SetWindowlessFrameRate(frame_rate_);
    std::cout << "[Client] Windowless frame rate set to " << frame_rate_ << " FPS\n";
  }

  // Force an invalidation to trigger paint events
  browser->GetHost()->Invalidate(PET_VIEW);
  std::cout << "[Client] Forced browser invalidation to trigger paint\n";

  // Real OpenVR pose -> page via iwerBridge.applyPose (browser-side injection)
  // In VR mode, pose updates are always enabled at 8ms intervals
  // In desktop mode, synthetic pose animation runs at 16ms intervals
  CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
  int pose_ms = 0;
  if (cmd.get() && cmd->HasSwitch("iwer-apply-pose"))
  {
    // Allow override of default timing
    pose_ms = (std::max)(5, atoi(cmd->GetSwitchValue("iwer-apply-pose").ToString().c_str()));
    if (pose_ms <= 0)
      pose_ms = 8;
  }
  else if (vr_mode_)
  {
    // Default VR mode: 8ms updates (~120Hz)
    pose_ms = 8;
  }
  const int desktop_interval_default = 16;
  if (pose_ms > 0 && !vr_mode_)
  {
    std::cout << "[Client] Ignoring --iwer-apply-pose in desktop mode (no OpenVR data available)\n";
    pose_ms = 0;
  }
  if (pose_ms > 0 && vr_mode_)
  {
    class VrPoseTask : public CefTask
    {
    public:
      VrPoseTask(CefRefPtr<OffscreenClient> c, int ms) : client_(c), ms_(ms) {}
      static void ToPosQuat(const vr::HmdMatrix34_t &m, float pos[3], float quat[4])
      {
        pos[0] = m.m[0][3];
        pos[1] = m.m[1][3];
        pos[2] = m.m[2][3];
        // 3x3 rotation -> quaternion (row-major)
        float r00 = m.m[0][0], r01 = m.m[0][1], r02 = m.m[0][2];
        float r10 = m.m[1][0], r11 = m.m[1][1], r12 = m.m[1][2];
        float r20 = m.m[2][0], r21 = m.m[2][1], r22 = m.m[2][2];
        float trace = r00 + r11 + r22;
        if (trace > 0.0f)
        {
          float S = sqrtf(trace + 1.0f) * 2.0f;
          quat[3] = 0.25f * S;       // w
          quat[0] = (r21 - r12) / S; // x
          quat[1] = (r02 - r20) / S; // y
          quat[2] = (r10 - r01) / S; // z
        }
        else if ((r00 > r11) && (r00 > r22))
        {
          float S = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
          quat[3] = (r21 - r12) / S;
          quat[0] = 0.25f * S;
          quat[1] = (r01 + r10) / S;
          quat[2] = (r02 + r20) / S;
        }
        else if (r11 > r22)
        {
          float S = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
          quat[3] = (r02 - r20) / S;
          quat[0] = (r01 + r10) / S;
          quat[1] = 0.25f * S;
          quat[2] = (r12 + r21) / S;
        }
        else
        {
          float S = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
          quat[3] = (r10 - r01) / S;
          quat[0] = (r02 + r20) / S;
          quat[1] = (r12 + r21) / S;
          quat[2] = 0.25f * S;
        }
      }
      static void Slerp(const float q1[4], const float q2[4], float t, float result[4])
      {
        float q2_copy[4] = {q2[0], q2[1], q2[2], q2[3]};
        float dot = q1[0] * q2_copy[0] + q1[1] * q2_copy[1] + q1[2] * q2_copy[2] + q1[3] * q2_copy[3];
        if (dot < 0.0f)
        {
          dot = -dot;
          q2_copy[0] = -q2_copy[0];
          q2_copy[1] = -q2_copy[1];
          q2_copy[2] = -q2_copy[2];
          q2_copy[3] = -q2_copy[3];
        }
        const float epsilon = 1e-6f;
        if (dot > 1.0f - epsilon)
        {
          // Nearly the same, linear interpolation
          for (int i = 0; i < 4; ++i)
            result[i] = (1.0f - t) * q1[i] + t * q2_copy[i];
          return;
        }
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float a = sinf((1.0f - t) * theta) / sin_theta;
        float b = sinf(t * theta) / sin_theta;
        for (int i = 0; i < 4; ++i)
          result[i] = a * q1[i] + b * q2_copy[i];
      }
      void Execute() override
      {
        CEF_REQUIRE_UI_THREAD();
        if (!client_.get())
          return;
        auto br = client_->GetBrowser();
        if (!br.get())
          return;
        auto frame = br->GetMainFrame();
        if (!frame.get())
          return;
        if (!vr::VRSystem())
        {
          CefPostDelayedTask(TID_UI, this, ms_);
          return;
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        // Sample predicted pose aligned with HMD vsync -> seconds-to-photons
        float sinceVsync = 0.0f; uint64_t fc = 0;
        vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
        float secondsFromVsyncToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float, &propErr);
        if (propErr != vr::TrackedProp_Success)
          secondsFromVsyncToPhotons = 0.0f;
        float displayHz = vr::VRSystem()->GetFloatTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propErr);
        if (propErr != vr::TrackedProp_Success || displayHz <= 0.0f)
          displayHz = 90.0f;
        const double frameDur = 1.0 / static_cast<double>(displayHz);
        double predictedSeconds = 0.0;
        if (vr::VRSystem()->GetTimeSinceLastVsync(&sinceVsync, &fc))
        {
          double untilNextVsync = frameDur - static_cast<double>(sinceVsync);
          if (untilNextVsync < 0.0) untilNextVsync = 0.0;
          predictedSeconds = untilNextVsync + static_cast<double>(secondsFromVsyncToPhotons);
        }
        // webxr pose
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding, static_cast<float>(predictedSeconds), poses, vr::k_unMaxTrackedDeviceCount);
        float hPos[3] = {0}, hQuat[4] = {0, 0, 0, 1};
        float lPos[3] = {0}, lQuat[4] = {0, 0, 0, 1};
        float rPos[3] = {0}, rQuat[4] = {0, 0, 0, 1};
        // HMD
        if (poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
        {
          ToPosQuat(poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking, hPos, hQuat);
        }
        // Controllers
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
        {
          if (!poses[i].bPoseIsValid)
            continue;
          if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
            continue;
          auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i);
          if (role == vr::TrackedControllerRole_LeftHand)
          {
            ToPosQuat(poses[i].mDeviceToAbsoluteTracking, lPos, lQuat);
          }
          else if (role == vr::TrackedControllerRole_RightHand)
          {
            ToPosQuat(poses[i].mDeviceToAbsoluteTracking, rPos, rQuat);
          }
        }
        // Apply blending to reduce jumps
        const float alpha = 0.5f; // Blend factor: higher = more smoothing
        if (!initialized_)
        {
          memcpy(prev_hPos, hPos, sizeof(hPos));
          memcpy(prev_hQuat, hQuat, sizeof(hQuat));
          memcpy(prev_lPos, lPos, sizeof(lPos));
          memcpy(prev_lQuat, lQuat, sizeof(lQuat));
          memcpy(prev_rPos, rPos, sizeof(rPos));
          memcpy(prev_rQuat, rQuat, sizeof(rQuat));
          initialized_ = true;
        }
        else
        {
          // Linear interpolation for positions
          for (int i = 0; i < 3; ++i)
          {
            hPos[i] = alpha * hPos[i] + (1.0f - alpha) * prev_hPos[i];
            lPos[i] = alpha * lPos[i] + (1.0f - alpha) * prev_lPos[i];
            rPos[i] = alpha * rPos[i] + (1.0f - alpha) * prev_rPos[i];
          }
          // Spherical linear interpolation for quaternions
          float blended_hQuat[4];
          Slerp(prev_hQuat, hQuat, alpha, blended_hQuat);
          memcpy(hQuat, blended_hQuat, sizeof(blended_hQuat));
          float blended_lQuat[4];
          Slerp(prev_lQuat, lQuat, alpha, blended_lQuat);
          memcpy(lQuat, blended_lQuat, sizeof(blended_lQuat));
          float blended_rQuat[4];
          Slerp(prev_rQuat, rQuat, alpha, blended_rQuat);
          memcpy(rQuat, blended_rQuat, sizeof(blended_rQuat));
        }
        // Update previous poses
        memcpy(prev_hPos, hPos, sizeof(hPos));
        memcpy(prev_hQuat, hQuat, sizeof(hQuat));
        memcpy(prev_lPos, lPos, sizeof(lPos));
        memcpy(prev_lQuat, lQuat, sizeof(lQuat));
        memcpy(prev_rPos, rPos, sizeof(rPos));
        memcpy(prev_rQuat, rQuat, sizeof(rQuat));
        client_->SendPoseToRenderer(frame, hPos, hQuat, lPos, lQuat, rPos, rQuat);
        // Nudge a paint to ensure the page renders with the freshest pose data
        br->GetHost()->Invalidate(PET_VIEW);
        // Dynamically align next tick near the next HMD vsync, but do not exceed configured pose_ms
        int nextDelayMs = ms_;
        if (vr::VRSystem())
        {
          vr::ETrackedPropertyError err = vr::TrackedProp_Success;
          float displayHz2 = vr::VRSystem()->GetFloatTrackedDeviceProperty(
              vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err);
          if (err != vr::TrackedProp_Success || displayHz2 <= 0.0f)
            displayHz2 = 90.0f;
          const double frameDurMs = 1000.0 / static_cast<double>(displayHz2);
          float sinceVsync2 = 0.0f; uint64_t fc2 = 0;
          if (vr::VRSystem()->GetTimeSinceLastVsync(&sinceVsync2, &fc2))
          {
            double untilNextVsyncMs = frameDurMs - static_cast<double>(sinceVsync2) * 1000.0;
            while (untilNextVsyncMs < 0.0) untilNextVsyncMs += frameDurMs;
            const double biasMs = 0.5; // submit shortly before vsync
            double ideal = (untilNextVsyncMs > biasMs) ? (untilNextVsyncMs - biasMs) : (frameDurMs - biasMs);
            ideal = std::max(1.0, ideal);
            nextDelayMs = static_cast<int>(std::min(ideal, static_cast<double>(ms_)));
          }
        }
        CefPostDelayedTask(TID_UI, this, nextDelayMs);
      }

    private:
      CefRefPtr<OffscreenClient> client_;
      int ms_;
      float prev_hPos[3] = {0.0f};
      float prev_hQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      float prev_lPos[3] = {0.0f};
      float prev_lQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      float prev_rPos[3] = {0.0f};
      float prev_rQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      bool initialized_ = false;
      IMPLEMENT_REFCOUNTING(VrPoseTask);
    };
    CefPostDelayedTask(TID_UI, new VrPoseTask(this, pose_ms), pose_ms);
  }

  // Log pose bridge info
  const int report_interval = vr_mode_ ? pose_ms : desktop_interval_default;
  std::cout << "[Client] IWER pose bridge enabled"
            << " (" << (vr_mode_ ? "OpenVR" : "desktop synthetic")
            << ", interval=" << report_interval << "ms)\n";

  // Desktop-mode synthetic pose: drive iwerBridge.applyPose via ExecuteJavaScript without renderer bridge.
  if (!vr_mode_)
  {
    std::cout << "[Client] Starting desktop dummy pose animation" << std::endl;
    class DesktopPoseTask : public CefTask
    {
    public:
      DesktopPoseTask(CefRefPtr<OffscreenClient> c, int interval_ms)
          : client_(c), interval_ms_(interval_ms), tick_(0) {}
      void Execute() override
      {
        CEF_REQUIRE_UI_THREAD();
        if (!client_.get())
          return;
        auto br = client_->GetBrowser();
        if (!br.get())
          return;
        if (!br->HasDocument())
        {
          CefPostDelayedTask(TID_UI, this, interval_ms_);
          return;
        }
        auto frame = br->GetMainFrame();
        if (!frame.get())
        {
          CefPostDelayedTask(TID_UI, this, interval_ms_);
          return;
        }
        ++tick_;
        const double t = tick_ * (interval_ms_ / 1000.0);
        const double orbit_radius = 0.40;
        const double x = orbit_radius * std::sin(t);
        const double y = 1.72 + 0.08 * std::sin(t * 0.45);
        const double z = -1.45 + orbit_radius * std::cos(t);
        const double yaw = 0.35 * std::sin(t * 0.7);
        const double half_yaw = yaw * 0.5;
        const double quat_x = 0.0;
        const double quat_y = std::sin(half_yaw);
        const double quat_z = 0.0;
        const double quat_w = std::cos(half_yaw);

        const double left_offset = -0.3 + 0.05 * std::sin(t * 1.3);
        const double right_offset = 0.3 + 0.05 * std::sin(t * 1.1);
        const double hand_y = 1.45 + 0.05 * std::cos(t * 0.6);
        const double hand_z = -1.3 + 0.05 * std::cos(t * 0.9);
        const double left_yaw = 0.25 * std::sin(t * 0.8);
        const double right_yaw = -0.25 * std::sin(t * 0.85);
        const double left_half = left_yaw * 0.5;
        const double right_half = right_yaw * 0.5;

        float hPos[3] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        float hQuat[4] = {static_cast<float>(quat_x), static_cast<float>(quat_y), static_cast<float>(quat_z), static_cast<float>(quat_w)};
        float leftPos[3] = {static_cast<float>(left_offset), static_cast<float>(hand_y), static_cast<float>(hand_z)};
        float leftQuat[4] = {static_cast<float>(std::sin(left_half)), 0.0f, 0.0f, static_cast<float>(std::cos(left_half))};
        float rightPos[3] = {static_cast<float>(right_offset), static_cast<float>(hand_y), static_cast<float>(hand_z)};
        float rightQuat[4] = {0.0f, 0.0f, static_cast<float>(std::sin(right_half)), static_cast<float>(std::cos(right_half))};

        const bool sent = client_->SendPoseToRenderer(frame, hPos, hQuat, leftPos, leftQuat, rightPos, rightQuat);
        if (!sent)
        {
          char js[2048];
          std::snprintf(js, sizeof(js),
                        "(function(){\n"
                        "  try {\n"
                        "    var state = {\n"
                        "      hmd:{pos:[%f,%f,%f], quat:[%f,%f,%f,%f]},\n"
                        "      left:{pos:[%f,%f,%f], quat:[%f,%f,%f,%f]},\n"
                        "      right:{pos:[%f,%f,%f], quat:[%f,%f,%f,%f]}\n"
                        "    };\n"
                        "    var g = (typeof window !== 'undefined') ? window : this;\n"
                        "    var bridge = g && g.iwerBridge;\n"
                        "    var apply = bridge && bridge.applyPose;\n"
                        "    if (apply) {\n"
                        "      var ok = apply(state);\n"
                        "      g.__desktopPoseCount = (g.__desktopPoseCount || 0) + 1;\n"
                        "      if (g.__desktopPoseCount === 1 || (g.__desktopPoseCount %% 120) === 0) {\n"
                        "        try { console.log('[desktop-pose] ok=', ok, 'pos=', state.hmd.pos); } catch(e){}\n"
                        "      }\n"
                        "    } else {\n"
                        "      g.__desktopPoseWarn = (g.__desktopPoseWarn || 0) + 1;\n"
                        "      if (g.__desktopPoseWarn === 1 || (g.__desktopPoseWarn %% 60) === 0) {\n"
                        "        try { console.warn('[desktop-pose] iwerBridge.applyPose missing'); } catch(e){}\n"
                        "      }\n"
                        "    }\n"
                        "  } catch(e) {\n"
                        "    try { console.error('[desktop-pose] error', e); } catch(_e){}\n"
                        "  }\n"
                        "})();",
                        x, y, z, quat_x, quat_y, quat_z, quat_w,
                        left_offset, hand_y, hand_z, std::sin(left_half), 0.0, 0.0, std::cos(left_half),
                        right_offset, hand_y, hand_z, 0.0, 0.0, std::sin(right_half), std::cos(right_half));
          frame->ExecuteJavaScript(js, "", 0);
        }
        CefPostDelayedTask(TID_UI, this, interval_ms_);
      }

    private:
      CefRefPtr<OffscreenClient> client_;
      int interval_ms_ = 16;
      int tick_;
      IMPLEMENT_REFCOUNTING(DesktopPoseTask);
    };
    CefPostDelayedTask(TID_UI, new DesktopPoseTask(this, desktop_interval_default), 200);
  }
}

void OffscreenClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Client] Browser closing... ID: " << browser->GetIdentifier() << "\n";
  browser_ = nullptr;
  // Signal that the browser is closed - this helps with shutdown
  if (host_window_)
  {
    PostMessage(host_window_, WM_DESTROY, 0, 0);
  }
  std::cout << "[Client] Browser closed\n";
}

void OffscreenClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect)
{
  rect = CefRect(0, 0, width_, height_);
  static bool logged = false;
  if (!logged)
  {
    std::cout << "[Client] GetViewRect called: " << width_ << "x" << height_ << "\n";
    logged = true;
  }
}

bool OffscreenClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info)
{
  screen_info.device_scale_factor = scale_;
  screen_info.depth = 24;
  screen_info.depth_per_component = 8;
  screen_info.is_monochrome = false;
  screen_info.rect = CefRect(0, 0, width_, height_);
  screen_info.available_rect = screen_info.rect;
  static bool logged = false;
  if (!logged)
  {
    std::cout << "[Client] GetScreenInfo called: scale=" << scale_ << ", depth=24\n";
    logged = true;
  }
  return true;
}

void OffscreenClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                                         PaintElementType type,
                                         const RectList &dirty_rects,
                                         const CefAcceleratedPaintInfo &info)
{
  CEF_REQUIRE_UI_THREAD();
  if (type != PET_VIEW)
    return;

  static int paint_count = 0;
  paint_count++;
  static auto first_paint_ts = std::chrono::steady_clock::now();
  static auto last_paint_ts = first_paint_ts;
  static auto last_report_ts = first_paint_ts;
  static double worst_frame_ms = 0.0;

  auto now = std::chrono::steady_clock::now();
  double delta_s = std::chrono::duration<double>(now - last_paint_ts).count();
  last_paint_ts = now;
  if (delta_s > 0.0)
  {
    double delta_ms = delta_s * 1000.0;
    if (delta_ms > worst_frame_ms)
    {
      worst_frame_ms = delta_ms;
    }
  }

  if (paint_count <= 10 || paint_count % 60 == 0)
  {
    std::cout << "[Client] OnAcceleratedPaint #" << paint_count << " - Handle: " << info.shared_texture_handle
              << ", Size: " << width_ << "x" << height_ << ", Format: " << info.format << "\n";
    std::cout << "[Client] Dirty rects count: " << dirty_rects.size() << "\n";
  }

  const bool interval_report = (paint_count % 120) == 0;
  const bool time_report = (std::chrono::duration<double>(now - last_report_ts).count() >= 5.0);
  if (paint_count == 1 || interval_report || time_report)
  {
    double total_s = std::chrono::duration<double>(now - first_paint_ts).count();
    double avg_hz = total_s > 0.0 ? static_cast<double>(paint_count) / total_s : 0.0;
    double avg_ms = avg_hz > 0.0 ? 1000.0 / avg_hz : 0.0;
    double last_ms = delta_s > 0.0 ? delta_s * 1000.0 : 0.0;
    std::cout << std::fixed << std::setprecision(2)
              << "[Client] Frame stats: total=" << paint_count
              << " avg=" << avg_hz << "Hz (" << avg_ms << "ms)"
              << " last=" << last_ms << "ms"
              << " worst=" << worst_frame_ms << "ms"
              << " dirty=" << dirty_rects.size()
              << std::defaultfloat << "\n";
    last_report_ts = now;
  }

  got_accel_.store(true, std::memory_order_relaxed);
  if (presenter_)
  {
    presenter_->PresentSharedHandle(info.shared_texture_handle, width_, height_);
  }
  else
  {
    std::cerr << "[Client] ERROR: No presenter available for OnAcceleratedPaint!\n";
  }
}

void OffscreenClient::OnPaint(CefRefPtr<CefBrowser> browser,
                              PaintElementType type,
                              const RectList &dirty_rects,
                              const void *buffer,
                              int width,
                              int height)
{
  // Fallback path when GPU is disabled; not implemented for brevity.
  static int software_paint_count = 0;
  software_paint_count++;
  std::cout << "[Client] OnPaint (software) #" << software_paint_count << " - Size: " << width << "x" << height << "\n";
  std::cout << "[Client] WARNING: Software rendering fallback - GPU acceleration may not be working!\n";
}

void OffscreenClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title)
{
  CEF_REQUIRE_UI_THREAD();
  std::cout << "[Client] Title changed: " << title.ToString() << "\n";
  if (host_window_)
    SetWindowTextW(host_window_, std::wstring(title).c_str());
}

bool OffscreenClient::SendPoseToRenderer(CefRefPtr<CefFrame> frame,
                                         const float *hmd_pos,
                                         const float *hmd_quat,
                                         const float *left_pos,
                                         const float *left_quat,
                                         const float *right_pos,
                                         const float *right_quat)
{
  if (!frame.get())
  {
    return false;
  }

  float payload[21] = {0};
  auto copy3 = [](float *dst, const float *src)
  {
    for (int i = 0; i < 3; ++i)
      dst[i] = src[i];
  };
  auto copy4 = [](float *dst, const float *src)
  {
    for (int i = 0; i < 4; ++i)
      dst[i] = src[i];
  };
  copy3(&payload[0], hmd_pos);
  copy4(&payload[3], hmd_quat);
  copy3(&payload[7], left_pos);
  copy4(&payload[10], left_quat);
  copy3(&payload[14], right_pos);
  copy4(&payload[17], right_quat);

  CefRefPtr<CefBinaryValue> bin = CefBinaryValue::Create(payload, sizeof(payload));
  CefRefPtr<CefProcessMessage> pm = CefProcessMessage::Create("VR_STATE");
  pm->GetArgumentList()->SetBinary(0, bin);
  frame->SendProcessMessage(PID_RENDERER, pm);
  return true;
}

bool OffscreenClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                       cef_log_severity_t level,
                                       const CefString &message,
                                       const CefString &source,
                                       int line)
{
  CEF_REQUIRE_UI_THREAD();
  const char *sev = "VERBOSE";
  switch (level)
  {
  case LOGSEVERITY_DEBUG:
    sev = "DEBUG";
    break;
  case LOGSEVERITY_INFO:
    sev = "INFO";
    break;
  case LOGSEVERITY_WARNING:
    sev = "WARN";
    break;
  case LOGSEVERITY_ERROR:
    sev = "ERROR";
    break;
  case LOGSEVERITY_FATAL:
    sev = "FATAL";
    break;
  default:
    break;
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
                                     bool is_redirect)
{
  return false; // allow
}

bool OffscreenClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               CefProcessId source_process,
                                               CefRefPtr<CefProcessMessage> message)
{
  CEF_REQUIRE_UI_THREAD();
  const std::string name = message->GetName();
  if (name == "RB_LOG")
  {
    auto args = message->GetArgumentList();
    std::string s = (args.get() && args->GetSize() > 0 && args->GetType(0) == VTYPE_STRING)
                        ? args->GetString(0).ToString()
                        : std::string();
    std::cout << "[Renderer->Browser][RB_LOG] " << s << "\n";
    return true; // handled
  }
  if (name == "RB_POSE")
  {
    auto args = message->GetArgumentList();
    double x = (args.get() && args->GetSize() > 0 && args->GetType(0) == VTYPE_DOUBLE) ? args->GetDouble(0) : 0.0;
    double y = (args.get() && args->GetSize() > 1 && args->GetType(1) == VTYPE_DOUBLE) ? args->GetDouble(1) : 0.0;
    double z = (args.get() && args->GetSize() > 2 && args->GetType(2) == VTYPE_DOUBLE) ? args->GetDouble(2) : 0.0;
    std::cout << "[Renderer->Browser][RB_POSE] hmd.pos = (" << x << ", " << y << ", " << z << ")\n";
    return true; // handled
  }
  return false;
}

void OffscreenClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                int httpStatusCode)
{
  CEF_REQUIRE_UI_THREAD();
  if (!frame.get() || !frame->IsMain())
    return;
}
