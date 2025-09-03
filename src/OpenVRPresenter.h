#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <openvr.h>
#include "Presenter.h"

// OpenVRPresenter: minimal presenter that submits CEF-rendered frames to an OpenVR overlay.
// Responsibilities:
// - Create a D3D11 device used to interop with CEF's shared textures
// - Create/find and configure an OpenVR overlay
// - Submit a legacy DXGI shared-handle texture each frame

class OpenVRPresenter : public Presenter {
public:
  OpenVRPresenter();
  ~OpenVRPresenter();

  // Initialize presenter and overlay. Non-throwing; returns false on failure.
  bool Initialize(const char* overlay_key, int width, int height, float scale = 1.0f);

  // Update cached size and overlay physical width-in-meters (scale).
  void Resize(int width, int height, float scale) override;

  // Submit a CEF shared texture HANDLE for this frame.
  // Handles keyed mutex acquisition (if present), copies/resolves to a reusable
  // legacy-shared texture, and submits via DXGI shared handle.
  void PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight) override;

  // Get the D3D11 device for CEF compatibility
  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const override { return device_; }

private:
  bool InitializeOpenVR();
  bool CreateD3DDevice();
  void Cleanup();

  // D3D11 device/context used for interop and copies.
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  
  // Overlay identification and handle.
  vr::VROverlayHandle_t overlay_handle_;
  std::string overlay_key_;
  
  // Source texture dimensions and overlay physical width scale (meters).
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
