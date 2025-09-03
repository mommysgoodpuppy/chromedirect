#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <openvr.h>
#include "Presenter.h"

class OpenVRPresenter : public Presenter {
public:
  OpenVRPresenter();
  ~OpenVRPresenter();

  bool Initialize(const char* overlay_key, int width, int height, float scale = 1.0f);
  void Resize(int width, int height, float scale) override;
  void PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight) override;

  // Get the D3D11 device for CEF compatibility
  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const override { return device_; }

private:
  bool InitializeOpenVR();
  bool CreateD3DDevice();
  void Cleanup();

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

  // Reusable legacy-shared texture for DXGI handle submission to avoid per-frame allocations.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_legacy_tex_;
  D3D11_TEXTURE2D_DESC shared_legacy_desc_ = {};
  HANDLE shared_legacy_handle_ = nullptr;
};
