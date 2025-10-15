#include "OpenVRPresenter.h"

#include <stdexcept>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <dxgi.h>
#include <d3dcompiler.h>

/*
 Minimal OpenVR overlay presenter for CEF OSR.
 Highlights:
 - Creates a D3D11 device; interops with CEF shared textures
 - Configures overlay (width = scale_, texture bounds, transform)
 - Submits via DXGI legacy shared handle
 - Drains overlay events and paces with WaitFrameSync
*/
using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr)
{
  if (FAILED(hr))
  {
    throw std::runtime_error("D3D call failed");
  }
}

OpenVRPresenter::OpenVRPresenter()
    : overlay_handle_(vr::k_ulOverlayHandleInvalid), width_(0), height_(0), scale_(1.0f),
      openvr_initialized_(false), overlay_created_(false)
{
  std::cout << "[OpenVR] OpenVRPresenter constructor\n";
}

OpenVRPresenter::~OpenVRPresenter()
{
  std::cout << "[OpenVR] OpenVRPresenter destructor\n";
  Cleanup();
}

bool OpenVRPresenter::Initialize(const char *overlay_key, int width, int height, float scale)
{
  std::cout << "[OpenVR] Initialize called: " << width << "x" << height << ", scale=" << scale << "\n";
  std::cout << "[OpenVR] Overlay key: " << overlay_key << "\n";

  std::lock_guard<std::mutex> lock(mtx_);
  overlay_key_ = overlay_key;
  width_ = width;
  height_ = height;
  scale_ = scale;

  try
  {
    if (!CreateD3DDevice())
    {
      std::cerr << "[OpenVR] ERROR: Failed to create D3D device\n";
      return false;
    }

    if (!InitializeOpenVR())
    {
      std::cerr << "[OpenVR] ERROR: Failed to initialize OpenVR\n";
      return false;
    }

    // Default viewport for potential shader path
    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    std::cout << "[OpenVR] Initialize successful\n";
    return true;
  }
  catch (const std::exception &e)
  {
    std::cerr << "[OpenVR] ERROR: Initialize exception: " << e.what() << "\n";
    return false;
  }
  catch (...)
  {
    std::cerr << "[OpenVR] ERROR: Initialize unknown exception\n";
    return false;
  }
}

bool OpenVRPresenter::CreateD3DDevice()
{
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

bool OpenVRPresenter::InitializeOpenVR()
{
  std::cout << "[OpenVR] Initializing OpenVR...\n";

  vr::EVRInitError eVRInitError = vr::VRInitError_None;
  vr::VR_Init(&eVRInitError, vr::VRApplication_Overlay);

  if (eVRInitError != vr::VRInitError_None)
  {
    std::cerr << "[OpenVR] ERROR: VR_Init failed: " << vr::VR_GetVRInitErrorAsEnglishDescription(eVRInitError) << "\n";
    return false;
  }

  openvr_initialized_ = true;
  std::cout << "[OpenVR] OpenVR initialized successfully\n";

  if (!vr::VROverlay())
  {
    std::cerr << "[OpenVR] ERROR: Failed to get overlay interface\n";
    return false;
  }

  // Create or find the overlay
  vr::EVROverlayError overlayError = vr::VROverlay()->FindOverlay(overlay_key_.c_str(), &overlay_handle_);
  if (overlayError != vr::VROverlayError_None)
  {
    // Overlay doesn't exist, create it
    overlayError = vr::VROverlay()->CreateOverlay(overlay_key_.c_str(), "CEF Web Overlay", &overlay_handle_);
    if (overlayError != vr::VROverlayError_None)
    {
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
    bounds.uMin = 0.0f;
    bounds.vMin = 0.0f;
    bounds.uMax = 1.0f;
    bounds.vMax = 1.0f;
    vr::VROverlay()->SetOverlayTextureBounds(overlay_handle_, &bounds);

    // Make overlay visible
    vr::VROverlay()->ShowOverlay(overlay_handle_);

    std::cout << "[OpenVR] Overlay configured: width=" << scale_ << "m, bounds set, visible\n";

    // Position it in front of the user
    vr::HmdMatrix34_t transform = {};
    transform.m[0][0] = 1.0f;
    transform.m[0][1] = 0.0f;
    transform.m[0][2] = 0.0f;
    transform.m[0][3] = 0.0f;
    transform.m[1][0] = 0.0f;
    transform.m[1][1] = 1.0f;
    transform.m[1][2] = 0.0f;
    transform.m[1][3] = 1.0f;
    transform.m[2][0] = 0.0f;
    transform.m[2][1] = 0.0f;
    transform.m[2][2] = 1.0f;
    transform.m[2][3] = -2.0f;
    vr::VROverlay()->SetOverlayTransformAbsolute(overlay_handle_, vr::TrackingUniverseStanding, &transform);
  }
  else
  {
    std::cout << "[OpenVR] Found existing overlay with handle: " << overlay_handle_ << "\n";
  }

  // Show the overlay
  vr::VROverlay()->ShowOverlay(overlay_handle_);
  overlay_created_ = true;

  return true;
}

void OpenVRPresenter::SetStereoPanorama(bool enable)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!overlay_created_)
    return;
  vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_StereoPanorama, enable);
  // Ensure regular panorama flag is off when stereo is on
  if (enable)
    vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_Panorama, false);
}

void OpenVRPresenter::ConfigurePanoramaShader(bool enable, float fovHalfRadians)
{
  std::lock_guard<std::mutex> lock(mtx_);
  shader_enabled_ = enable;
  fov_half_radians_ = fovHalfRadians;
}

void OpenVRPresenter::SetWarpFollowHead(bool enable)
{
  std::lock_guard<std::mutex> lock(mtx_);
  warp_follow_head_ = enable;
}

void OpenVRPresenter::SetPremultipliedAlpha(bool enable)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!overlay_created_)
    return;
  vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_IsPremultiplied, enable);
}

void OpenVRPresenter::SetIgnoreTextureAlpha(bool enable)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!overlay_created_)
    return;
  vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_IgnoreTextureAlpha, enable);
}

static const char *kVS_Src = R"HLSL(
struct VSOut { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
static const float2 kPos[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
static const float2 kUV[4]  = { float2(0,0),   float2(1,0),   float2(0,1),  float2(1,1) };
VSOut main(uint vid:SV_VertexID){
  VSOut o;
  o.pos = float4(kPos[vid], 0.0f, 1.0f);
  // Flip V to match original shader's input convention
  o.uv = float2(kUV[vid].x, 1.0f - kUV[vid].y);
  return o;
}
)HLSL";

static const char *kPS_Src = R"HLSL(
Texture2D srcTex : register(t0);
SamplerState samp0 : register(s0);

cbuffer Params : register(b0) {
  float4x4 lookRotation; // view rotation
  float halfFOVInRadians;
  float applyRotation; // 0=no, 1=yes
  float2 pad;
}

static const float PI = 3.141592;
static const float HALF_PI = 0.5 * PI;
static const float QUARTER_PI = 0.25 * PI;

struct PSIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
float4 main(PSIn i) : SV_Target {
  // VS already provided normalized UV with V flipped
  float2 uv = i.uv;

  float2 xy = uv;
  float2 xy_normalized = 2.0 * xy - 1.0;
  float2 xy_angles = xy_normalized * float2(PI, HALF_PI);

  float2 xy_eye_angles = xy_angles;
  xy_eye_angles.y *= 2.0;
  bool renderTopHalf = (xy_eye_angles.y >= 0.0);
  if (renderTopHalf) xy_eye_angles.y -= HALF_PI; else xy_eye_angles.y += HALF_PI;

  float fovScalar = tan(halfFOVInRadians) / tan(QUARTER_PI);

  // Spherical direction
  float3 dir;
  dir.x = sin(xy_angles.x) * cos(xy_eye_angles.y);
  dir.y = sin(xy_eye_angles.y);
  dir.z = cos(xy_angles.x) * cos(xy_eye_angles.y);

  // Optional look rotation
  if (applyRotation > 0.5) {
    dir = mul(float4(dir,0.0), lookRotation).xyz;
  }

  float projX = (dir.x / abs(dir.z)) / fovScalar;
  float projY = (dir.y / abs(dir.z)) / fovScalar;
  float2 eyeUV = float2((projX + 1.0) * 0.5, (projY + 1.0) * 0.5);
  eyeUV = saturate(eyeUV);

  // Sample from SBS source: left half for top, right half for bottom
  if (renderTopHalf) {
    eyeUV.x = eyeUV.x * 0.5; // left half
  } else {
    eyeUV.x = eyeUV.x * 0.5 + 0.5; // right half
  }

  return srcTex.Sample(samp0, eyeUV);
}
)HLSL";

bool OpenVRPresenter::EnsureShaderPipeline(UINT outWidth, UINT outHeight)
{
  if (!shader_enabled_)
    return false;
  if (!device_ || !context_)
    return false;

  HRESULT hr;
  if (!vs_ || !ps_)
  {
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    hr = D3DCompile(kVS_Src, strlen(kVS_Src), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr))
    {
      std::cerr << "[OpenVR] ERROR: VS compile failed\n";
      return false;
    }
    hr = D3DCompile(kPS_Src, strlen(kPS_Src), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, psBlob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr))
    {
      std::cerr << "[OpenVR] ERROR: PS compile failed\n";
      return false;
    }
    ThrowIfFailed(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs_.GetAddressOf()));
    ThrowIfFailed(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps_.GetAddressOf()));

    // Constant buffer (4x4 matrix + 4 floats -> 80 bytes; align to 16)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 64 + 16; // 4x4 matrix (64) + 4 floats (packed into 16 due to 16-byte alignment)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&cbd, nullptr, cb_params_.GetAddressOf()));

    // Sampler (use anisotropic to preserve detail when remapping)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 16;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&sd, sampler_.GetAddressOf()));

    // Rasterizer: cull none
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ThrowIfFailed(device_->CreateRasterizerState(&rd, rs_state_.GetAddressOf()));

    // Blend: no blending, write all
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device_->CreateBlendState(&bd, blend_state_.GetAddressOf()));

    // Depth-stencil: disabled
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    ThrowIfFailed(device_->CreateDepthStencilState(&dd, ds_state_.GetAddressOf()));
  }

  // Ensure shared legacy tex is RTV-capable for direct render
  bool needTex = (!shared_legacy_tex_) ||
                 (shared_legacy_desc_.Width != outWidth) ||
                 (shared_legacy_desc_.Height != outHeight);
  if (needTex)
  {
    shared_legacy_tex_.Reset();
    shared_rtv_.Reset();
    shared_legacy_handle_ = nullptr;
    ZeroMemory(&shared_legacy_desc_, sizeof(shared_legacy_desc_));
    shared_legacy_desc_.Width = outWidth;
    shared_legacy_desc_.Height = outHeight;
    shared_legacy_desc_.MipLevels = 1;
    shared_legacy_desc_.ArraySize = 1;
    shared_legacy_desc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    shared_legacy_desc_.SampleDesc.Count = 1;
    shared_legacy_desc_.Usage = D3D11_USAGE_DEFAULT;
    shared_legacy_desc_.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    shared_legacy_desc_.CPUAccessFlags = 0;
    shared_legacy_desc_.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hrc = device_->CreateTexture2D(&shared_legacy_desc_, nullptr, shared_legacy_tex_.GetAddressOf());
    if (FAILED(hrc))
    {
      std::cerr << "[OpenVR] ERROR: CreateTexture2D RTV shared failed, hr=0x" << std::hex << hrc << std::dec << "\n";
      return false;
    }
    ThrowIfFailed(device_->CreateRenderTargetView(shared_legacy_tex_.Get(), nullptr, shared_rtv_.GetAddressOf()));
    ComPtr<IDXGIResource> dxr;
    if (SUCCEEDED(shared_legacy_tex_.As(&dxr)))
    {
      dxr->GetSharedHandle(&shared_legacy_handle_);
    }
  }

  viewport_.Width = static_cast<float>(outWidth);
  viewport_.Height = static_cast<float>(outHeight);
  return true;
}

void OpenVRPresenter::PresentSharedHandle(HANDLE shared_handle, int srcWidth, int srcHeight)
{
  std::lock_guard<std::mutex> lock(mtx_);

  static int present_count = 0;
  present_count++;
  static auto first_present_ts = std::chrono::steady_clock::now();
  static auto last_present_ts = first_present_ts;
  static auto last_report_ts = first_present_ts;
  static double worst_present_ms = 0.0;

  auto now = std::chrono::steady_clock::now();
  double delta_s = std::chrono::duration<double>(now - last_present_ts).count();
  last_present_ts = now;
  if (delta_s > 0.0)
  {
    double delta_ms = delta_s * 1000.0;
    if (delta_ms > worst_present_ms)
    {
      worst_present_ms = delta_ms;
    }
  }

  const bool interval_report = (present_count % 120) == 0;
  const bool time_report = (std::chrono::duration<double>(now - last_report_ts).count() >= 5.0);
  if (present_count == 1 || interval_report || time_report)
  {
    double total_s = std::chrono::duration<double>(now - first_present_ts).count();
    double avg_hz = total_s > 0.0 ? static_cast<double>(present_count) / total_s : 0.0;
    double avg_ms = avg_hz > 0.0 ? 1000.0 / avg_hz : 0.0;
    double last_ms = delta_s > 0.0 ? delta_s * 1000.0 : 0.0;
    std::cout << std::fixed << std::setprecision(2)
              << "[OpenVR] Submit stats: total=" << present_count
              << " avg=" << avg_hz << "Hz (" << avg_ms << "ms)"
              << " last=" << last_ms << "ms"
              << " worst=" << worst_present_ms << "ms"
              << std::defaultfloat << "\n";
    last_report_ts = now;
  }

  if (!device_ || !context_ || !openvr_initialized_ || !overlay_created_)
  {
    std::cerr << "[OpenVR] ERROR: Not properly initialized\n";
    return;
  }

  if (!shared_handle)
  {
    std::cerr << "[OpenVR] ERROR: Null shared handle\n";
    return;
  }

  if (present_count <= 5 || present_count % 60 == 0)
  {
    std::cout << "[OpenVR] PresentSharedHandle #" << present_count << " - Handle: " << shared_handle
              << ", Size: " << srcWidth << "x" << srcHeight << "\n";
  }

  // Open the CEF shared texture
  ComPtr<ID3D11Texture2D> sharedTex;
  {
    // Try OpenSharedResource1 first for newer DXGI handles
    ComPtr<ID3D11Device1> device1;
    if (SUCCEEDED(device_.As(&device1)))
    {
      HRESULT hr = device1->OpenSharedResource1(shared_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(sharedTex.GetAddressOf()));
      if (FAILED(hr))
      {
        if (present_count <= 3)
        {
          std::cout << "[OpenVR] OpenSharedResource1 failed (0x" << std::hex << hr << std::dec << "), trying legacy method\n";
        }
        // Fallback to legacy method
        ComPtr<ID3D11Resource> res;
        hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void **>(res.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
          res.As(&sharedTex);
        }
        else
        {
          std::cerr << "[OpenVR] ERROR: Both OpenSharedResource methods failed, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
          return;
        }
      }
      else if (present_count <= 3)
      {
        std::cout << "[OpenVR] OpenSharedResource1 succeeded\n";
      }
    }
    else
    {
      // Fallback to legacy method for older D3D11 devices
      ComPtr<ID3D11Resource> res;
      HRESULT hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Resource), reinterpret_cast<void **>(res.GetAddressOf()));
      if (SUCCEEDED(hr))
      {
        res.As(&sharedTex);
      }
      else
      {
        std::cerr << "[OpenVR] ERROR: Failed to open shared resource, HRESULT: 0x" << std::hex << hr << std::dec << "\n";
        return;
      }
    }
  }

  if (!sharedTex)
  {
    std::cerr << "[OpenVR] ERROR: Failed to get shared texture\n";
    return;
  }

  // Log texture desc for diagnostics
  D3D11_TEXTURE2D_DESC srcDesc = {};
  sharedTex->GetDesc(&srcDesc);
  if (present_count <= 3)
  {
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
  if (SUCCEEDED(sharedTex.As(&keyedMutex)))
  {
    HRESULT acquireResult = keyedMutex->AcquireSync(0, 50);
    if (SUCCEEDED(acquireResult))
    {
      acquiredKey = 0;
      if (present_count <= 3)
        std::cout << "[OpenVR] Keyed mutex acquired with key 0\n";
    }
    else
    {
      acquireResult = keyedMutex->AcquireSync(1, 50);
      if (SUCCEEDED(acquireResult))
      {
        acquiredKey = 1;
        if (present_count <= 3)
          std::cout << "[OpenVR] Keyed mutex acquired with key 1\n";
      }
      else if (present_count <= 5)
      {
        std::cerr << "[OpenVR] WARNING: Failed to acquire keyed mutex with key 0 or 1, hr=0x" << std::hex << acquireResult << std::dec << "\n";
      }
    }
  }

  // Decide which texture to submit: ensure SRV-capable and non-MSAA
  ComPtr<ID3D11Texture2D> submitTex = sharedTex;
  bool needsCopy = (srcDesc.SampleDesc.Count > 1) || ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0);
  if (needsCopy)
  {
    // Reuse persistent copy texture to avoid per-frame allocations
    bool needNewCopy = (!copy_tex_) ||
                       (copy_desc_.Width != srcDesc.Width) ||
                       (copy_desc_.Height != srcDesc.Height) ||
                       (copy_desc_.Format != srcDesc.Format);
    if (needNewCopy)
    {
      copy_tex_.Reset();
      ZeroMemory(&copy_desc_, sizeof(copy_desc_));
      copy_desc_.Width = srcDesc.Width;
      copy_desc_.Height = srcDesc.Height;
      copy_desc_.MipLevels = 1;
      copy_desc_.ArraySize = 1;
      copy_desc_.Format = srcDesc.Format;
      copy_desc_.SampleDesc.Count = 1;
      copy_desc_.SampleDesc.Quality = 0;
      copy_desc_.Usage = D3D11_USAGE_DEFAULT;
      copy_desc_.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      copy_desc_.CPUAccessFlags = 0;
      copy_desc_.MiscFlags = 0;
      HRESULT crt = device_->CreateTexture2D(&copy_desc_, nullptr, copy_tex_.GetAddressOf());
      if (FAILED(crt))
      {
        std::cerr << "[OpenVR] ERROR: Failed to (re)create copy texture, hr=0x" << std::hex << crt << std::dec << "\n";
        // Fall back to submitting the shared texture directly
        submitTex = sharedTex;
      }
    }
    if (copy_tex_)
    {
      if (srcDesc.SampleDesc.Count > 1)
      {
        // Resolve MSAA into single-sample
        context_->ResolveSubresource(copy_tex_.Get(), 0, sharedTex.Get(), 0, srcDesc.Format);
        context_->Flush();
        if (present_count <= 3)
          std::cout << "[OpenVR] Resolved MSAA texture for overlay submit\n";
      }
      else
      {
        context_->CopyResource(copy_tex_.Get(), sharedTex.Get());
        context_->Flush();
        if (present_count <= 3)
          std::cout << "[OpenVR] Copied texture into SRV-capable texture for overlay submit\n";
      }
      submitTex = copy_tex_;
    }
  }

  // Create or reuse the shared texture target. If shader path is enabled, ensure RTV target exists.
  D3D11_TEXTURE2D_DESC submitDesc = {};
  submitTex->GetDesc(&submitDesc);

  if (shader_enabled_)
  {
    // Ensure output surface matches overlay/presenter target size (width_, height_)
    if (!EnsureShaderPipeline(static_cast<UINT>(width_), static_cast<UINT>(height_)))
    {
      std::cerr << "[OpenVR] WARNING: Shader pipeline unavailable; falling back to copy\n";
      shader_enabled_ = false; // disable for subsequent frames
    }
  }

  if (!shader_enabled_)
  {
    // Copy path (previous behavior)
    const bool sizeChanged = (shared_legacy_tex_ == nullptr) ||
                             (shared_legacy_desc_.Width != submitDesc.Width) ||
                             (shared_legacy_desc_.Height != submitDesc.Height) ||
                             (shared_legacy_desc_.Format != submitDesc.Format);
    if (sizeChanged)
    {
      shared_legacy_tex_.Reset();
      shared_legacy_handle_ = nullptr;
      shared_legacy_desc_ = submitDesc;
      shared_legacy_desc_.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // legacy shared handle
      shared_legacy_desc_.BindFlags |= D3D11_BIND_SHADER_RESOURCE; // ensure SRV-capable
      HRESULT hrShare = device_->CreateTexture2D(&shared_legacy_desc_, nullptr, shared_legacy_tex_.GetAddressOf());
      if (FAILED(hrShare))
      {
        std::cerr << "[OpenVR] ERROR: Failed to create/recreate legacy-shared texture, hr=0x" << std::hex << hrShare << std::dec << "\n";
        // Fall back to direct pointer submission
        last_submitted_texture_ = submitTex;
        vr::Texture_t eyeTexture = {(void *)submitTex.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
        vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
        if (overlayError != vr::VROverlayError_None)
        {
          std::cerr << "[OpenVR] ERROR: SetOverlayTexture failed: " << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
        }
        else if (present_count <= 5)
        {
          std::cout << "[OpenVR] Texture submitted to overlay successfully (direct pointer fallback)\n";
        }
        goto post_submit_visibility_check;
      }
      // Fetch shared handle once
      ComPtr<IDXGIResource> dxgiRes;
      if (SUCCEEDED(shared_legacy_tex_.As(&dxgiRes)))
      {
        dxgiRes->GetSharedHandle(&shared_legacy_handle_);
      }
    }
    context_->CopyResource(shared_legacy_tex_.Get(), submitTex.Get());
    context_->Flush();
    last_submitted_texture_ = shared_legacy_tex_;
  }
  else
  {
    // Shader path: render SBS -> stereo panorama into shared_legacy_tex_
    // Create SRV for submitTex (let D3D infer view desc)
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hrs = device_->CreateShaderResourceView(submitTex.Get(), nullptr, srv.GetAddressOf());
    if (FAILED(hrs))
    {
      std::cerr << "[OpenVR] ERROR: Create SRV failed, hr=0x" << std::hex << hrs << std::dec << "\n";
      goto post_submit_visibility_check;
    }

    context_->OMSetRenderTargets(1, shared_rtv_.GetAddressOf(), nullptr);
    context_->RSSetViewports(1, &viewport_);
    context_->IASetInputLayout(nullptr);
    context_->RSSetState(rs_state_.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(blend_state_.Get(), blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(ds_state_.Get(), 0);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->PSSetShaderResources(0, 1, srv.GetAddressOf());

    // Update constants (supply a yaw-only look rotation from HMD if available)
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(context_->Map(cb_params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
      // Identity lookRotation, then parameters
      float *dst = reinterpret_cast<float *>(map.pData);
      // Default to identity
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          dst[r * 4 + c] = (r == c) ? 1.0f : 0.0f;
      // Query HMD pose and generate inverse yaw rotation if enabled
      bool appliedRotation = false;
      if (warp_follow_head_ && vr::VRSystem())
      {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (hmdPose.bPoseIsValid)
        {
          const vr::HmdMatrix34_t &m = hmdPose.mDeviceToAbsoluteTracking;
          // Extract yaw from 3x3 rotation (assuming column-major from OpenVR)
          // OpenVR's 3x4 matrix m: rows 0..2, cols 0..3
          // Forward Z axis components: m[2][0], m[2][2]
          float forwardX = m.m[0][2];
          float forwardZ = m.m[2][2];
          float yaw = atan2f(forwardX, forwardZ);
          float cy = cosf(-yaw); // inverse yaw
          float sy = sinf(-yaw);
          // Row-major 4x4
          dst[0] = cy;
          dst[1] = 0.0f;
          dst[2] = sy;
          dst[3] = 0.0f;
          dst[4] = 0.0f;
          dst[5] = 1.0f;
          dst[6] = 0.0f;
          dst[7] = 0.0f;
          dst[8] = -sy;
          dst[9] = 0.0f;
          dst[10] = cy;
          dst[11] = 0.0f;
          dst[12] = 0.0f;
          dst[13] = 0.0f;
          dst[14] = 0.0f;
          dst[15] = 1.0f;
          appliedRotation = true;
        }
      }
      dst[16] = fov_half_radians_;
      dst[17] = appliedRotation ? 1.0f : 0.0f; // applyRotation flag
      dst[18] = 0.0f;
      dst[19] = 0.0f;
      context_->Unmap(cb_params_.Get(), 0);
    }
    context_->VSSetConstantBuffers(0, 1, cb_params_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, cb_params_.GetAddressOf());

    // Draw full-screen quad (no VB/IB via SV_VertexID)
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->Draw(4, 0);
    context_->Flush();

    if (present_count <= 3)
    {
      std::cout << "[OpenVR] Shader panorama path drew fullscreen quad\n";
    }

    last_submitted_texture_ = shared_legacy_tex_;
  }

  if (shared_legacy_handle_)
  {
    vr::Texture_t eyeTexture = {(void *)shared_legacy_handle_, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Auto};
    vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
    if (overlayError != vr::VROverlayError_None)
    {
      std::cerr << "[OpenVR] ERROR: SetOverlayTexture (DXGISharedHandle) failed: "
                << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
    }
    else if (present_count <= 5)
    {
      std::cout << "[OpenVR] Texture submitted to overlay successfully (DXGI shared handle)\n";
    }
  }

post_submit_visibility_check:
  if (present_count <= 3)
  {
    bool visible = vr::VROverlay()->IsOverlayVisible(overlay_handle_);
    std::cout << "[OpenVR] Overlay visible: " << (visible ? "true" : "false") << "\n";
    if (!visible)
    {
      vr::VROverlay()->ShowOverlay(overlay_handle_);
      std::cout << "[OpenVR] Overlay was hidden; ShowOverlay called\n";
    }
  }

  // Release keyed mutex if we acquired it; set opposite key for producer/consumer handoff
  if (keyedMutex && acquiredKey != UINT64_MAX)
  {
    UINT64 releaseKey = (acquiredKey == 0) ? 1 : 0;
    keyedMutex->ReleaseSync(releaseKey);
  }

  // Drain overlay events to keep the queue from growing
  vr::VREvent_t evt;
  while (vr::VROverlay()->PollNextOverlayEvent(overlay_handle_, &evt, sizeof(evt)))
  {
    // No-op: could add minimal logging for unusual events
  }

  // Optionally pace submissions with the runtime's update rate to avoid spamming.
  // This blocks until the top of frame (or timeout). Keep timeout conservative to avoid long UI stalls.
  vr::VROverlay()->WaitFrameSync(0.1);
}

void OpenVRPresenter::Resize(int width, int height, float scale)
{
  std::lock_guard<std::mutex> lock(mtx_);
  width_ = width;
  height_ = height;
  scale_ = scale;
  std::cout << "[OpenVR] Resized to: " << width << "x" << height << ", scale=" << scale << "\n";
  if (overlay_created_ && overlay_handle_ != vr::k_ulOverlayHandleInvalid)
  {
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, scale_);
  }
}

void OpenVRPresenter::Cleanup()
{
  std::lock_guard<std::mutex> lock(mtx_);

  if (overlay_created_ && overlay_handle_ != vr::k_ulOverlayHandleInvalid)
  {
    std::cout << "[OpenVR] Destroying overlay...\n";
    vr::VROverlay()->DestroyOverlay(overlay_handle_);
    overlay_handle_ = vr::k_ulOverlayHandleInvalid;
    overlay_created_ = false;
  }

  if (openvr_initialized_)
  {
    std::cout << "[OpenVR] Shutting down OpenVR...\n";
    vr::VR_Shutdown();
    openvr_initialized_ = false;
  }

  // Release shared legacy resources
  shared_legacy_tex_.Reset();
  shared_legacy_handle_ = nullptr;
  ZeroMemory(&shared_legacy_desc_, sizeof(shared_legacy_desc_));
  last_submitted_texture_.Reset();
  copy_tex_.Reset();
  ZeroMemory(&copy_desc_, sizeof(copy_desc_));

  context_.Reset();
  device_.Reset();
}
