#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <mutex>

class D3DPresenter {
public:
  D3DPresenter();
  ~D3DPresenter();

  bool Initialize(HWND hwnd, int width, int height, float scale = 1.0f);
  void Resize(int width, int height, float scale = 1.0f);

  // Present a shared texture handle coming from CEF OnAcceleratedPaint.
  // width/height are the source texture size in pixels.
  void PresentSharedHandle(HANDLE shared_handle, int width, int height);

  HWND GetHwnd() const { return hwnd_; }
  ID3D11Device* GetDevice() const { return device_.Get(); }

private:
  bool CreateDeviceSwapchain();
  void CreateRenderTarget();
  void ReleaseRenderTarget();

private:
  HWND hwnd_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  float scale_ = 1.0f;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

  std::mutex mtx_;
};
