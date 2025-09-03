#include "OpenVRPresenter.h"

#include <stdexcept>
#include <iostream>
#include <dxgi.h>

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
    
    // Set overlay properties (use scale_ as width in meters)
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, scale_);
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

  // (debug red texture scheduling removed)

  // Acquire keyed mutex if present (try key 0, then 1). Release with the same key we acquired.
  ComPtr<IDXGIKeyedMutex> keyedMutex;
  UINT64 acquiredKey = UINT64_MAX;
  if (SUCCEEDED(sharedTex.As(&keyedMutex))) {
    HRESULT acquireResult = keyedMutex->AcquireSync(0, 50);
    if (SUCCEEDED(acquireResult)) {
      acquiredKey = 0;
      if (present_count <= 3) std::cout << "[OpenVR] Keyed mutex acquired with key 0\n";
    } else {
      acquireResult = keyedMutex->AcquireSync(1, 50);
      if (SUCCEEDED(acquireResult)) {
        acquiredKey = 1;
        if (present_count <= 3) std::cout << "[OpenVR] Keyed mutex acquired with key 1\n";
      } else if (present_count <= 5) {
        std::cerr << "[OpenVR] WARNING: Failed to acquire keyed mutex with key 0 or 1, hr=0x" << std::hex << acquireResult << std::dec << "\n";
      }
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
        context_->Flush();
        if (present_count <= 3) std::cout << "[OpenVR] Resolved MSAA texture for overlay submit\n";
      } else {
        context_->CopyResource(dstTex.Get(), sharedTex.Get());
        context_->Flush();
        if (present_count <= 3) std::cout << "[OpenVR] Copied texture into SRV-capable texture for overlay submit\n";
      }
      submitTex = dstTex;
    }
  }

  // Create or reuse a legacy-shared (non-NT handle) texture and copy the contents, then submit via DXGI shared handle
  D3D11_TEXTURE2D_DESC submitDesc = {};
  submitTex->GetDesc(&submitDesc);
  const bool sizeChanged = (shared_legacy_tex_ == nullptr) ||
                           (shared_legacy_desc_.Width != submitDesc.Width) ||
                           (shared_legacy_desc_.Height != submitDesc.Height) ||
                           (shared_legacy_desc_.Format != submitDesc.Format);
  if (sizeChanged) {
    shared_legacy_tex_.Reset();
    shared_legacy_handle_ = nullptr;
    shared_legacy_desc_ = submitDesc;
    shared_legacy_desc_.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // legacy shared handle
    shared_legacy_desc_.BindFlags |= D3D11_BIND_SHADER_RESOURCE; // ensure SRV-capable
    HRESULT hrShare = device_->CreateTexture2D(&shared_legacy_desc_, nullptr, shared_legacy_tex_.GetAddressOf());
    if (FAILED(hrShare)) {
      std::cerr << "[OpenVR] ERROR: Failed to create/recreate legacy-shared texture, hr=0x" << std::hex << hrShare << std::dec << "\n";
      // Fall back to direct pointer submission
      last_submitted_texture_ = submitTex;
      vr::Texture_t eyeTexture = { (void*)submitTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto };
      vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
      if (overlayError != vr::VROverlayError_None) {
        std::cerr << "[OpenVR] ERROR: SetOverlayTexture failed: " << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
      } else if (present_count <= 5) {
        std::cout << "[OpenVR] Texture submitted to overlay successfully (direct pointer fallback)\n";
      }
      goto post_submit_visibility_check; // skip DXGI handle path this frame
    }
    // Fetch shared handle once
    ComPtr<IDXGIResource> dxgiRes;
    if (FAILED(shared_legacy_tex_.As(&dxgiRes))) {
      std::cerr << "[OpenVR] ERROR: Failed to QI IDXGIResource on legacy-shared texture\n";
    } else {
      HRESULT hrHandle = dxgiRes->GetSharedHandle(&shared_legacy_handle_);
      if (FAILED(hrHandle) || !shared_legacy_handle_) {
        std::cerr << "[OpenVR] ERROR: GetSharedHandle failed for legacy-shared texture, hr=0x" << std::hex << hrHandle << std::dec << "\n";
      } else if (present_count <= 3) {
        std::cout << "[OpenVR] Obtained DXGI shared handle: " << shared_legacy_handle_ << "\n";
      }
    }
  }

  if (shared_legacy_tex_) {
    context_->CopyResource(shared_legacy_tex_.Get(), submitTex.Get());
    context_->Flush();
    last_submitted_texture_ = shared_legacy_tex_; // ensure lifetime across submit
    if (shared_legacy_handle_) {
      vr::Texture_t eyeTexture = { (void*)shared_legacy_handle_, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Auto };
      vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
      if (overlayError != vr::VROverlayError_None) {
        std::cerr << "[OpenVR] ERROR: SetOverlayTexture (DXGISharedHandle) failed: "
                  << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
      } else if (present_count <= 5) {
        std::cout << "[OpenVR] Texture submitted to overlay successfully (DXGI shared handle)\n";
      }
    }
  }

post_submit_visibility_check:
  if (present_count <= 3) {
    bool visible = vr::VROverlay()->IsOverlayVisible(overlay_handle_);
    std::cout << "[OpenVR] Overlay visible: " << (visible ? "true" : "false") << "\n";
    if (!visible) {
      vr::VROverlay()->ShowOverlay(overlay_handle_);
      std::cout << "[OpenVR] Overlay was hidden; ShowOverlay called\n";
    }
  }

  // Release keyed mutex if we acquired it; set opposite key for producer/consumer handoff
  if (keyedMutex && acquiredKey != UINT64_MAX) {
    UINT64 releaseKey = (acquiredKey == 0) ? 1 : 0;
    keyedMutex->ReleaseSync(releaseKey);
  }

  // Drain overlay events to keep the queue from growing
  vr::VREvent_t evt;
  while (vr::VROverlay()->PollNextOverlayEvent(overlay_handle_, &evt, sizeof(evt))) {
    // No-op: could add minimal logging for unusual events
  }

  // Optionally pace submissions with the runtime's update rate to avoid spamming.
  // This blocks until the top of frame (or timeout). Keep timeout conservative to avoid long UI stalls.
  vr::VROverlay()->WaitFrameSync(100);
}

void OpenVRPresenter::Resize(int width, int height, float scale) {
  std::lock_guard<std::mutex> lock(mtx_);
  width_ = width;
  height_ = height;
  scale_ = scale;
  std::cout << "[OpenVR] Resized to: " << width << "x" << height << ", scale=" << scale << "\n";
  if (overlay_created_ && overlay_handle_ != vr::k_ulOverlayHandleInvalid) {
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, scale_);
  }
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
  
  // Release shared legacy resources
  shared_legacy_tex_.Reset();
  shared_legacy_handle_ = nullptr;
  ZeroMemory(&shared_legacy_desc_, sizeof(shared_legacy_desc_));
  last_submitted_texture_.Reset();

  context_.Reset();
  device_.Reset();
}

