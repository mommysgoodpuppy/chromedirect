#include "D3DPresenter.h"

#include <stdexcept>
#include <vector>
#include <iostream>

using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr) {
  if (FAILED(hr)) {
    throw std::runtime_error("D3D call failed");
  }
}

D3DPresenter::D3DPresenter() {
  std::cout << "[D3D] D3DPresenter constructor\n";
}
D3DPresenter::~D3DPresenter() {
  std::cout << "[D3D] D3DPresenter destructor\n";
  std::lock_guard<std::mutex> lock(mtx_);
  ReleaseRenderTarget();
  swap_chain_.Reset();
  context_.Reset();
  device_.Reset();
}

bool D3DPresenter::Initialize(HWND hwnd, int width, int height, float scale) {
  std::cout << "[D3D] Initialize called: " << width << "x" << height << ", scale=" << scale << "\n";
  std::lock_guard<std::mutex> lock(mtx_);
  hwnd_ = hwnd;
  width_ = width;
  height_ = height;
  scale_ = scale;
  try {
    if (!CreateDeviceSwapchain()) {
      std::cerr << "[D3D] ERROR: CreateDeviceSwapchain failed\n";
      return false;
    }
    CreateRenderTarget();
    std::cout << "[D3D] Initialize successful\n";
  } catch (const std::exception& e) {
    std::cerr << "[D3D] ERROR: Initialize exception: " << e.what() << "\n";
    return false;
  } catch (...) {
    std::cerr << "[D3D] ERROR: Initialize unknown exception\n";
    return false;
  }
  return true;
}

void D3DPresenter::Resize(int width, int height, float scale) {
  std::lock_guard<std::mutex> lock(mtx_);
  width_ = width;
  height_ = height;
  scale_ = scale;
  if (!swap_chain_) return;
  ReleaseRenderTarget();
  swap_chain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
  CreateRenderTarget();
}

bool D3DPresenter::CreateDeviceSwapchain() {
  std::cout << "[D3D] Creating D3D11 device and swapchain...\n";
  
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
  std::cout << "[D3D] Debug layer enabled\n";
#endif
  D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = width_;
  sd.BufferDesc.Height = height_;
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd_;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  ComPtr<IDXGIFactory> factory;
  ThrowIfFailed(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf())));
  std::cout << "[D3D] DXGI factory created\n";

  D3D_FEATURE_LEVEL actualFeatureLevel;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                 featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), &actualFeatureLevel, context_.GetAddressOf());
  ThrowIfFailed(hr);
  std::cout << "[D3D] D3D11 device created with feature level: " << std::hex << actualFeatureLevel << std::dec << "\n";

  ThrowIfFailed(factory->CreateSwapChain(device_.Get(), &sd, swap_chain_.GetAddressOf()));
  std::cout << "[D3D] Swapchain created successfully\n";
  return true;
}

void D3DPresenter::CreateRenderTarget() {
  ComPtr<ID3D11Texture2D> backBuffer;
  ThrowIfFailed(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())));
  ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv_.GetAddressOf()));
}

void D3DPresenter::ReleaseRenderTarget() {
  rtv_.Reset();
}

void D3DPresenter::PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight) {
  std::lock_guard<std::mutex> lock(mtx_);
  
  static int present_count = 0;
  present_count++;
  
  if (!device_ || !context_ || !swap_chain_ || !rtv_) {
    std::cerr << "[D3D] ERROR: Missing D3D resources in PresentSharedHandle\n";
    return;
  }
  
  if (!shared_handle) {
    std::cerr << "[D3D] ERROR: Null shared handle in PresentSharedHandle\n";
    return;
  }

  if (present_count <= 5 || present_count % 60 == 0) {
    std::cout << "[D3D] PresentSharedHandle #" << present_count << " - Handle: " << shared_handle 
              << ", Size: " << srcWidth << "x" << srcHeight << "\n";
  }

  // Try to open shared handle - CEF uses DXGI keyed mutex shared textures
  ComPtr<ID3D11Texture2D> sharedTex;
  {
    // First try OpenSharedResource1 for newer DXGI handles
    ComPtr<ID3D11Device1> device1;
    if (SUCCEEDED(device_.As(&device1))) {
      HRESULT hr = device1->OpenSharedResource1(shared_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(sharedTex.GetAddressOf()));
      if (FAILED(hr)) {
        if (present_count <= 3) {
          std::cout << "[D3D] OpenSharedResource1 failed (0x" << std::hex << hr << std::dec << "), trying legacy method\n";
        }
        // Fallback to legacy method
        ComPtr<ID3D11Resource> res;
        hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void**>(res.GetAddressOf()));
        if (SUCCEEDED(hr)) {
          res.As(&sharedTex);
        } else {
          std::cerr << "[D3D] ERROR: Both OpenSharedResource methods failed, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
          return;
        }
      } else if (present_count <= 3) {
        std::cout << "[D3D] OpenSharedResource1 succeeded\n";
      }
    } else {
      // Fallback to legacy method for older D3D11 devices
      ComPtr<ID3D11Resource> res;
      HRESULT hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void**>(res.GetAddressOf()));
      if (SUCCEEDED(hr)) {
        res.As(&sharedTex);
      } else {
        std::cerr << "[D3D] ERROR: Failed to open shared resource, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
        return;
      }
    }
  }
  if (!sharedTex) {
    std::cerr << "[D3D] ERROR: Failed to get shared texture\n";
    return;
  }

  // Acquire keyed mutex if present (CEF uses keyed mutex for synchronization)
  ComPtr<IDXGIKeyedMutex> keyedMutex;
  if (SUCCEEDED(sharedTex.As(&keyedMutex))) {
    HRESULT acquireResult = keyedMutex->AcquireSync(0, 100); // Wait up to 100ms
    if (FAILED(acquireResult)) {
      if (present_count <= 5) {
        std::cerr << "[D3D] WARNING: Failed to acquire keyed mutex, HRESULT: 0x" << std::hex << acquireResult << std::dec << "\n";
      }
      // Continue anyway - some shared textures don't use keyed mutex
    } else if (present_count <= 3) {
      std::cout << "[D3D] Keyed mutex acquired\n";
    }
  }

  // Clear and copy to backbuffer
  const float clearColor[4] = {0.2f, 0.2f, 0.8f, 1.0f}; // Blue background to see if D3D is working
  context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  context_->ClearRenderTargetView(rtv_.Get(), clearColor);

  ComPtr<ID3D11Texture2D> backBuffer;
  if (SUCCEEDED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
    // If sizes differ, do a stretch via shader would be ideal. For simplicity, CopySubresourceRegion when equal size, otherwise still copy.
    D3D11_TEXTURE2D_DESC srcDesc = {};
    sharedTex->GetDesc(&srcDesc);
    D3D11_TEXTURE2D_DESC dstDesc = {};
    backBuffer->GetDesc(&dstDesc);

    if (present_count <= 3) {
      std::cout << "[D3D] Texture sizes - Src: " << srcDesc.Width << "x" << srcDesc.Height 
                << ", Dst: " << dstDesc.Width << "x" << dstDesc.Height << "\n";
    }

    if (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height) {
      context_->CopyResource(backBuffer.Get(), sharedTex.Get());
      if (present_count <= 3) {
        std::cout << "[D3D] Using CopyResource (exact size match)\n";
      }
    } else {
      D3D11_BOX box = {};
      box.left = 0; box.top = 0; box.front = 0;
      box.right = srcDesc.Width; box.bottom = srcDesc.Height; box.back = 1;
      context_->CopySubresourceRegion(backBuffer.Get(), 0, 0, 0, 0, sharedTex.Get(), 0, &box);
      if (present_count <= 3) {
        std::cout << "[D3D] Using CopySubresourceRegion (size mismatch)\n";
      }
    }
  } else {
    std::cerr << "[D3D] ERROR: Failed to get back buffer\n";
    return;
  }

  // Release keyed mutex if we acquired it
  if (keyedMutex) {
    keyedMutex->ReleaseSync(0);
  }

  HRESULT presentResult = swap_chain_->Present(1, 0);
  if (FAILED(presentResult)) {
    std::cerr << "[D3D] ERROR: Present failed, HRESULT: 0x" << std::hex << presentResult << std::dec << "\n";
  } else if (present_count <= 5) {
    std::cout << "[D3D] Present successful\n";
  }
}

