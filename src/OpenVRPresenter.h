#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <openvr.h>
#include "Presenter.h"

// OpenVRPresenter: minimal presenter that submits CEF-rendered frames to an OpenVR overlay.
// Responsibilities:
// - Create a D3D11 device used to interop with CEF's shared textures
// - Create/find and configure an OpenVR overlay
// - Submit a legacy DXGI shared-handle texture each frame

class OpenVRPresenter : public Presenter
{
public:
  OpenVRPresenter();
  ~OpenVRPresenter();

  // Initialize presenter and overlay. Non-throwing; returns false on failure.
  bool Initialize(const char *overlay_key, int width, int height, float scale = 1.0f);

  // Update cached size and overlay physical width-in-meters (scale).
  void Resize(int width, int height, float scale) override;

  // Submit a CEF shared texture HANDLE for this frame.
  // Handles keyed mutex acquisition (if present), copies/resolves to a reusable
  // legacy-shared texture, and submits via DXGI shared handle.
  void PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight) override;

  // Configure overlay flags
  void SetStereoPanorama(bool enable);
  void SetPremultipliedAlpha(bool enable);
  void SetIgnoreTextureAlpha(bool enable);

  // Configure shader-based panorama transform (from SBS input)
  // fovHalfRadians: half FOV in radians for the projection.
  void SetFOVHalfRadians(float fovHalfRadians);
  void SetWarpFollowHead(bool enable);

  // Get the D3D11 device for CEF compatibility
  Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const override { return device_; }

private:
  bool InitializeOpenVR();
  bool CreateD3DDevice();
  bool EnsureShaderPipeline(UINT outWidth, UINT outHeight);
  void Cleanup();
  void StartRenderLoop();
  void StopRenderLoop();
  void RenderLoop();
  void UpdateHMDTimingProperties();
  bool AcquirePredictedPose(vr::HmdMatrix34_t &pose);

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

  // Reusable copy texture when the source is MSAA or not SRV-capable to avoid per-frame CreateTexture2D churn.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_tex_;
  D3D11_TEXTURE2D_DESC copy_desc_ = {};
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copy_srv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_source_tex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> last_source_srv_;

  // Shader pipeline for rendering into shared_legacy_tex_
  // Default to ~56 degrees half-FOV (matches prior pipeline: 112° vertical FOV total)
  float fov_half_radians_ = 0.9773843811168246f; // 56 deg in radians
  // Follow HMD yaw by default to keep panorama stable relative to the overlay (HUD-like)
  bool warp_follow_head_ = true;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_params_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> shared_rtv_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_state_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> ds_state_;
  D3D11_VIEWPORT viewport_ = {};
  std::thread render_thread_;
  std::atomic<bool> render_running_{false};
  float display_frequency_hz_ = 90.0f;
  float seconds_from_vsync_to_photons_ = 0.0f;
  bool hmd_timing_props_cached_ = false;
  uint64_t last_vsync_frame_counter_ = 0;
  bool have_last_vsync_frame_counter_ = false;
  uint64_t frames_skipped_debug_ = 0;
  // debug fields removed
};
