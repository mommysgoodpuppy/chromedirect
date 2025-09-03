#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include <openvr.h>

class OpenVRPresenter {
public:
  OpenVRPresenter();
  ~OpenVRPresenter();

  bool Initialize(const char* overlay_key, int width, int height, float scale = 1.0f);
  void Resize(int width, int height, float scale);
  void PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight);

  // Get the D3D11 device for CEF compatibility
  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const { return device_; }

private:
  bool InitializeOpenVR();
  bool CreateD3DDevice();
  void Cleanup();

  // Schedules a timed submission of a solid color texture to the overlay for debugging.
  void ScheduleTestTextureAfterDelay(UINT width, UINT height, DXGI_FORMAT format, int delay_ms);
  void SubmitSolidColorTexture(UINT width, UINT height, DXGI_FORMAT format, uint32_t rgba);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  
  vr::VROverlayHandle_t overlay_handle_;
  std::string overlay_key_;
  
  int width_;
  int height_;
  float scale_;
  
  std::mutex mtx_;
  bool openvr_initialized_;
  bool overlay_created_;

  // Keep a reference to the most recently submitted texture to ensure lifetime across frames.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_submitted_texture_;

  // Ensure we only schedule the test texture once.
  std::atomic<bool> test_texture_timer_started_{false};
};
