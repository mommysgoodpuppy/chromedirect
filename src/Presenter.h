#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

class Presenter {
public:
  virtual ~Presenter() = default;

  // Submit a GPU shared HANDLE produced by CEF OSR.
  virtual void PresentSharedHandle(HANDLE shared_handle, int width, int height) = 0;

  // Resize notifications from app/browser.
  virtual void Resize(int width, int height, float scale) = 0;

  // Optional: provide the D3D11 device used for interop with CEF.
  virtual Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const = 0;
};
