#include "OpenVRPresenter.h"

#include <stdexcept>
#include <iostream>
#include <thread>
#include <chrono>

using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr) {
  if (FAILED(hr)) {
    throw std::runtime_error("D3D call failed");
  }
}

OpenVRPresenter::OpenVRPresenter() 
  : overlay_handle_(vr::k_ulOverlayHandleInvalid), width_(0), height_(0), scale_(1.0f), 
    openvr_initialized_(false), overlay_created_(false) {
  std::cout << "[OpenVR] OpenVRPresenter constructor\n";
}

OpenVRPresenter::~OpenVRPresenter() {
  std::cout << "[OpenVR] OpenVRPresenter destructor\n";
  Cleanup();
}

bool OpenVRPresenter::Initialize(const char* overlay_key, int width, int height, float scale) {
  std::cout << "[OpenVR] Initialize called: " << width << "x" << height << ", scale=" << scale << "\n";
  std::cout << "[OpenVR] Overlay key: " << overlay_key << "\n";
  
  std::lock_guard<std::mutex> lock(mtx_);
  overlay_key_ = overlay_key;
  width_ = width;
  height_ = height;
  scale_ = scale;
  
  try {
    if (!CreateD3DDevice()) {
      std::cerr << "[OpenVR] ERROR: Failed to create D3D device\n";
      return false;
    }
    
    if (!InitializeOpenVR()) {
      std::cerr << "[OpenVR] ERROR: Failed to initialize OpenVR\n";
      return false;
    }
    
    std::cout << "[OpenVR] Initialize successful\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[OpenVR] ERROR: Initialize exception: " << e.what() << "\n";
    return false;
  } catch (...) {
    std::cerr << "[OpenVR] ERROR: Initialize unknown exception\n";
    return false;
  }
}

bool OpenVRPresenter::CreateD3DDevice() {
  std::cout << "[OpenVR] Creating D3D11 device...\n";
  
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
  std::cout << "[OpenVR] Debug layer enabled\n";
#endif

  D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  D3D_FEATURE_LEVEL actualFeatureLevel;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                 featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), &actualFeatureLevel, context_.GetAddressOf());
  ThrowIfFailed(hr);
  std::cout << "[OpenVR] D3D11 device created with feature level: " << std::hex << actualFeatureLevel << std::dec << "\n";
  return true;
}

bool OpenVRPresenter::InitializeOpenVR() {
  std::cout << "[OpenVR] Initializing OpenVR...\n";
  
  vr::EVRInitError eVRInitError = vr::VRInitError_None;
  vr::VR_Init(&eVRInitError, vr::VRApplication_Overlay);
  
  if (eVRInitError != vr::VRInitError_None) {
    std::cerr << "[OpenVR] ERROR: VR_Init failed: " << vr::VR_GetVRInitErrorAsEnglishDescription(eVRInitError) << "\n";
    return false;
  }
  
  openvr_initialized_ = true;
  std::cout << "[OpenVR] OpenVR initialized successfully\n";
  
  if (!vr::VROverlay()) {
    std::cerr << "[OpenVR] ERROR: Failed to get overlay interface\n";
    return false;
  }
  
  // Create or find the overlay
  vr::EVROverlayError overlayError = vr::VROverlay()->FindOverlay(overlay_key_.c_str(), &overlay_handle_);
  if (overlayError != vr::VROverlayError_None) {
    // Overlay doesn't exist, create it
    overlayError = vr::VROverlay()->CreateOverlay(overlay_key_.c_str(), "CEF Web Overlay", &overlay_handle_);
    if (overlayError != vr::VROverlayError_None) {
      std::cerr << "[OpenVR] ERROR: Failed to create overlay: " << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
      return false;
    }
    std::cout << "[OpenVR] Overlay created with handle: " << overlay_handle_ << "\n";
    
    // Set overlay properties
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, 2.0f);
    vr::VROverlay()->SetOverlayAlpha(overlay_handle_, 1.0f);
    vr::VROverlay()->SetOverlayColor(overlay_handle_, 1.0f, 1.0f, 1.0f);
    
    // Set texture bounds to match our expected size
    vr::VRTextureBounds_t bounds;
    bounds.uMin = 0.0f; bounds.vMin = 0.0f;
    bounds.uMax = 1.0f; bounds.vMax = 1.0f;
    vr::VROverlay()->SetOverlayTextureBounds(overlay_handle_, &bounds);
    
    // Make overlay visible
    vr::VROverlay()->ShowOverlay(overlay_handle_);
    
    std::cout << "[OpenVR] Overlay configured: 2.0m width, bounds set, visible\n";
    
    // Position it in front of the user
    vr::HmdMatrix34_t transform = {};
    transform.m[0][0] = 1.0f; transform.m[0][1] = 0.0f; transform.m[0][2] = 0.0f; transform.m[0][3] = 0.0f;
    transform.m[1][0] = 0.0f; transform.m[1][1] = 1.0f; transform.m[1][2] = 0.0f; transform.m[1][3] = 1.0f;
    transform.m[2][0] = 0.0f; transform.m[2][1] = 0.0f; transform.m[2][2] = 1.0f; transform.m[2][3] = -2.0f;
    vr::VROverlay()->SetOverlayTransformAbsolute(overlay_handle_, vr::TrackingUniverseStanding, &transform);
    
  } else {
    std::cout << "[OpenVR] Found existing overlay with handle: " << overlay_handle_ << "\n";
  }
  
  // Show the overlay
  vr::VROverlay()->ShowOverlay(overlay_handle_);
  overlay_created_ = true;
  
  return true;
}

void OpenVRPresenter::PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight) {
  std::lock_guard<std::mutex> lock(mtx_);
  
  static int present_count = 0;
  present_count++;
  
  if (!device_ || !context_ || !openvr_initialized_ || !overlay_created_) {
    std::cerr << "[OpenVR] ERROR: Not properly initialized\n";
    return;
  }
  
  if (!shared_handle) {
    std::cerr << "[OpenVR] ERROR: Null shared handle\n";
    return;
  }

  if (present_count <= 5 || present_count % 60 == 0) {
    std::cout << "[OpenVR] PresentSharedHandle #" << present_count << " - Handle: " << shared_handle 
              << ", Size: " << srcWidth << "x" << srcHeight << "\n";
  }

  // Open the CEF shared texture
  ComPtr<ID3D11Texture2D> sharedTex;
  {
    // Try OpenSharedResource1 first for newer DXGI handles
    ComPtr<ID3D11Device1> device1;
    if (SUCCEEDED(device_.As(&device1))) {
      HRESULT hr = device1->OpenSharedResource1(shared_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(sharedTex.GetAddressOf()));
      if (FAILED(hr)) {
        if (present_count <= 3) {
          std::cout << "[OpenVR] OpenSharedResource1 failed (0x" << std::hex << hr << std::dec << "), trying legacy method\n";
        }
        // Fallback to legacy method
        ComPtr<ID3D11Resource> res;
        hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void**>(res.GetAddressOf()));
        if (SUCCEEDED(hr)) {
          res.As(&sharedTex);
        } else {
          std::cerr << "[OpenVR] ERROR: Both OpenSharedResource methods failed, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
          return;
        }
      } else if (present_count <= 3) {
        std::cout << "[OpenVR] OpenSharedResource1 succeeded\n";
      }
    } else {
      // Fallback to legacy method for older D3D11 devices
      ComPtr<ID3D11Resource> res;
      HRESULT hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void**>(res.GetAddressOf()));
      if (SUCCEEDED(hr)) {
        res.As(&sharedTex);
      } else {
        std::cerr << "[OpenVR] ERROR: Failed to open shared resource, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
        return;
      }
    }
  }
  
  if (!sharedTex) {
    std::cerr << "[OpenVR] ERROR: Failed to get shared texture\n";
    return;
  }

  // Log texture desc for diagnostics
  D3D11_TEXTURE2D_DESC srcDesc = {};
  sharedTex->GetDesc(&srcDesc);
  if (present_count <= 3) {
    std::cout << "[OpenVR] SharedTex Desc - "
              << "Size: " << srcDesc.Width << "x" << srcDesc.Height
              << ", Mips: " << srcDesc.MipLevels
              << ", Array: " << srcDesc.ArraySize
              << ", Format: 0x" << std::hex << srcDesc.Format << std::dec
              << ", BindFlags: 0x" << std::hex << srcDesc.BindFlags << std::dec
              << ", MiscFlags: 0x" << std::hex << srcDesc.MiscFlags << std::dec
              << ", SampleCount: " << srcDesc.SampleDesc.Count
              << "\n";
  }

  // Schedule a test red texture submission once after a short delay for debugging overlay pipeline
  if (!test_texture_timer_started_.load(std::memory_order_relaxed)) {
    test_texture_timer_started_.store(true, std::memory_order_relaxed);
    ScheduleTestTextureAfterDelay(srcDesc.Width, srcDesc.Height, srcDesc.Format, 3000);
  }

  // Acquire keyed mutex if present
  ComPtr<IDXGIKeyedMutex> keyedMutex;
  if (SUCCEEDED(sharedTex.As(&keyedMutex))) {
    // Consumer pattern: acquire key 1 (producer releases with 1), release with 0
    HRESULT acquireResult = keyedMutex->AcquireSync(1, 100); // Wait up to 100ms
    if (FAILED(acquireResult)) {
      if (present_count <= 5) {
        std::cerr << "[OpenVR] WARNING: Failed to acquire keyed mutex, HRESULT: 0x" << std::hex << acquireResult << std::dec << "\n";
      }
      // Continue anyway - some shared textures don't use keyed mutex
    } else if (present_count <= 3) {
      std::cout << "[OpenVR] Keyed mutex acquired\n";
    }
  }

  // Decide which texture to submit: ensure SRV-capable and non-MSAA
  ComPtr<ID3D11Texture2D> submitTex = sharedTex;
  bool needsCopy = (srcDesc.SampleDesc.Count > 1) || ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0);
  if (needsCopy) {
    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = srcDesc.Format;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.SampleDesc.Quality = 0;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dstDesc.CPUAccessFlags = 0;
    dstDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> dstTex;
    HRESULT crt = device_->CreateTexture2D(&dstDesc, nullptr, dstTex.GetAddressOf());
    if (FAILED(crt)) {
      std::cerr << "[OpenVR] ERROR: Failed to create SRV-capable staging texture, hr=0x" << std::hex << crt << std::dec << "\n";
      // Fall back to submitting the shared texture directly
      submitTex = sharedTex;
    } else {
      if (srcDesc.SampleDesc.Count > 1) {
        // Resolve MSAA into single-sample
        context_->ResolveSubresource(dstTex.Get(), 0, sharedTex.Get(), 0, srcDesc.Format);
        if (present_count <= 3) std::cout << "[OpenVR] Resolved MSAA texture for overlay submit\n";
      } else {
        context_->CopyResource(dstTex.Get(), sharedTex.Get());
        if (present_count <= 3) std::cout << "[OpenVR] Copied texture into SRV-capable texture for overlay submit\n";
      }
      submitTex = dstTex;
    }
  }

  // Hold reference to ensure lifetime across SetOverlayTexture
  last_submitted_texture_ = submitTex;

  // Submit texture to OpenVR overlay
  vr::Texture_t eyeTexture = { (void*)submitTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto };
  vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
  
  if (overlayError != vr::VROverlayError_None) {
    std::cerr << "[OpenVR] ERROR: SetOverlayTexture failed: " << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
  } else if (present_count <= 5) {
    std::cout << "[OpenVR] Texture submitted to overlay successfully\n";
  }

  if (present_count <= 3) {
    bool visible = vr::VROverlay()->IsOverlayVisible(overlay_handle_);
    std::cout << "[OpenVR] Overlay visible: " << (visible ? "true" : "false") << "\n";
    if (!visible) {
      vr::VROverlay()->ShowOverlay(overlay_handle_);
      std::cout << "[OpenVR] Overlay was hidden; ShowOverlay called\n";
    }
  }

  // Release keyed mutex if we acquired it
  if (keyedMutex) {
    keyedMutex->ReleaseSync(0);
  }
}

void OpenVRPresenter::Resize(int width, int height, float scale) {
  std::lock_guard<std::mutex> lock(mtx_);
  width_ = width;
  height_ = height;
  scale_ = scale;
  std::cout << "[OpenVR] Resized to: " << width << "x" << height << ", scale=" << scale << "\n";
}

void OpenVRPresenter::Cleanup() {
  std::lock_guard<std::mutex> lock(mtx_);
  
  if (overlay_created_ && overlay_handle_ != vr::k_ulOverlayHandleInvalid) {
    std::cout << "[OpenVR] Destroying overlay...\n";
    vr::VROverlay()->DestroyOverlay(overlay_handle_);
    overlay_handle_ = vr::k_ulOverlayHandleInvalid;
    overlay_created_ = false;
  }
  
  if (openvr_initialized_) {
    std::cout << "[OpenVR] Shutting down OpenVR...\n";
    vr::VR_Shutdown();
    openvr_initialized_ = false;
  }
  
  context_.Reset();
  device_.Reset();
}

void OpenVRPresenter::ScheduleTestTextureAfterDelay(UINT width, UINT height, DXGI_FORMAT format, int delay_ms) {
  // Fire-and-forget background task
  std::thread([this, width, height, format, delay_ms]() {
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      // Double-check state before submitting
      {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!openvr_initialized_ || !overlay_created_ || !device_ || !context_) {
          std::cout << "[OpenVR] Test texture skipped: not initialized anymore\n";
          return;
        }
      }
      std::cout << "[OpenVR] Submitting timed test red texture...\n";
      SubmitSolidColorTexture(width, height, format, 0xFF0000FFu); // RGBA red (we'll clear via RTV anyway)
    } catch (...) {
      // Swallow exceptions in background thread to avoid crashes
    }
  }).detach();
}

void OpenVRPresenter::SubmitSolidColorTexture(UINT width, UINT height, DXGI_FORMAT format, uint32_t rgba) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (!device_ || !context_ || !openvr_initialized_ || !overlay_created_) {
    std::cerr << "[OpenVR] ERROR: SubmitSolidColorTexture: Not properly initialized\n";
    return;
  }

  // Create a render-target-capable texture
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> tex;
  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "[OpenVR] ERROR: SubmitSolidColorTexture: CreateTexture2D failed, hr=0x" << std::hex << hr << std::dec << "\n";
    return;
  }

  // Create RTV and clear to red
  ComPtr<ID3D11RenderTargetView> rtv;
  hr = device_->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "[OpenVR] ERROR: SubmitSolidColorTexture: CreateRenderTargetView failed, hr=0x" << std::hex << hr << std::dec << "\n";
    return;
  }

  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  context_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  D3D11_VIEWPORT vp{};
  vp.TopLeftX = 0.0f;
  vp.TopLeftY = 0.0f;
  vp.Width = static_cast<float>(width);
  vp.Height = static_cast<float>(height);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &vp);
  context_->ClearRenderTargetView(rtv.Get(), red);

  // Submit
  last_submitted_texture_ = tex;
  vr::Texture_t eyeTexture = { (void*)tex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto };
  vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
  if (overlayError != vr::VROverlayError_None) {
    std::cerr << "[OpenVR] ERROR: SubmitSolidColorTexture: SetOverlayTexture failed: "
              << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
  } else {
    std::cout << "[OpenVR] SubmitSolidColorTexture: Red texture submitted successfully\n";
  }
}
