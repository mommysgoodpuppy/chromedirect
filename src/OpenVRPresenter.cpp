#include "OpenVRPresenter.h"

#include <stdexcept>
#include <iostream>

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

  // Acquire keyed mutex if present
  ComPtr<IDXGIKeyedMutex> keyedMutex;
  if (SUCCEEDED(sharedTex.As(&keyedMutex))) {
    HRESULT acquireResult = keyedMutex->AcquireSync(0, 100); // Wait up to 100ms
    if (FAILED(acquireResult)) {
      if (present_count <= 5) {
        std::cerr << "[OpenVR] WARNING: Failed to acquire keyed mutex, HRESULT: 0x" << std::hex << acquireResult << std::dec << "\n";
      }
      // Continue anyway - some shared textures don't use keyed mutex
    } else if (present_count <= 3) {
      std::cout << "[OpenVR] Keyed mutex acquired\n";
    }
  }

  // Submit texture to OpenVR overlay
  vr::Texture_t eyeTexture = { (void*)sharedTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma };
  vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
  
  if (overlayError != vr::VROverlayError_None) {
    std::cerr << "[OpenVR] ERROR: SetOverlayTexture failed: " << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
  } else if (present_count <= 5) {
    std::cout << "[OpenVR] Texture submitted to overlay successfully\n";
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
