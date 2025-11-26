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

static void BuildLookRotationMatrix(const vr::HmdMatrix34_t &pose, float *dst16)
{
  if (!dst16)
    return;
  const float r00 = pose.m[0][0];
  const float r01 = pose.m[0][1];
  const float r02 = pose.m[0][2];
  const float r10 = pose.m[1][0];
  const float r11 = pose.m[1][1];
  const float r12 = pose.m[1][2];
  const float r20 = pose.m[2][0];
  const float r21 = pose.m[2][1];
  const float r22 = pose.m[2][2];
  // lookRotation = transpose(scale(inverse(universeFromHmd), vec3(1,1,-1))) = scaleRows(R, {1,1,-1})
  dst16[0] = r00;  dst16[1] = r01;  dst16[2] = r02;  dst16[3] = 0.0f;
  dst16[4] = r10;  dst16[5] = r11;  dst16[6] = r12;  dst16[7] = 0.0f;
  dst16[8] = -r20; dst16[9] = -r21; dst16[10] = -r22; dst16[11] = 0.0f;
  dst16[12] = 0.0f; dst16[13] = 0.0f; dst16[14] = 0.0f; dst16[15] = 1.0f;
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
    StartRenderLoop();
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

  if (vr::VRCompositor())
  {
    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);
  }

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

    // We perform panorama mapping in our own shader; disable compositor panorama processing
    vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_Panorama, false);
    vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_StereoPanorama, true);

    // Make overlay visible
    vr::VROverlay()->ShowOverlay(overlay_handle_);

    std::cout << "[OpenVR] Overlay configured: width=" << scale_ << "m, bounds set, visible\n";

    // Position relative to HMD (HUD-like), ~1m forward to reduce parallax
    vr::HmdMatrix34_t transform = {};
    transform.m[0][0] = 1.0f;
    transform.m[0][1] = 0.0f;
    transform.m[0][2] = 0.0f;
    transform.m[0][3] = 0.0f;
    transform.m[1][0] = 0.0f;
    transform.m[1][1] = 1.0f;
    transform.m[1][2] = 0.0f;
    transform.m[1][3] = 0.0f;
    transform.m[2][0] = 0.0f;
    transform.m[2][1] = 0.0f;
    transform.m[2][2] = 1.0f;
    transform.m[2][3] = -1.0f;
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        overlay_handle_, vr::k_unTrackedDeviceIndex_Hmd, &transform);
  }
  else
  {
    std::cout << "[OpenVR] Found existing overlay with handle: " << overlay_handle_ << "\n";
    // Ensure flags and bounds are correct even for existing overlays
    vr::VRTextureBounds_t bounds;
    bounds.uMin = 0.0f;
    bounds.vMin = 0.0f;
    bounds.uMax = 1.0f;
    bounds.vMax = 1.0f;
    vr::VROverlay()->SetOverlayTextureBounds(overlay_handle_, &bounds);
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, scale_);
    vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_Panorama, false);
    vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_StereoPanorama, true);
    // Keep it HMD-relative at -1m Z
    vr::HmdMatrix34_t transform = {};
    transform.m[0][0] = 1.0f;
    transform.m[0][1] = 0.0f;
    transform.m[0][2] = 0.0f;
    transform.m[0][3] = 0.0f;
    transform.m[1][0] = 0.0f;
    transform.m[1][1] = 1.0f;
    transform.m[1][2] = 0.0f;
    transform.m[1][3] = 0.0f;
    transform.m[2][0] = 0.0f;
    transform.m[2][1] = 0.0f;
    transform.m[2][2] = 1.0f;
    transform.m[2][3] = -1.0f;
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        overlay_handle_, vr::k_unTrackedDeviceIndex_Hmd, &transform);
  }

  // Show the overlay
  vr::VROverlay()->ShowOverlay(overlay_handle_);
  overlay_created_ = true;

  UpdateHMDTimingProperties();

  return true;
}

void OpenVRPresenter::UpdateHMDTimingProperties()
{
  if (!vr::VRSystem())
    return;
  vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
  float displayHz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propErr);
  if (propErr == vr::TrackedProp_Success && displayHz > 0.0f)
  {
    display_frequency_hz_ = displayHz;
  }
  propErr = vr::TrackedProp_Success;
  float secondsToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float, &propErr);
  if (propErr == vr::TrackedProp_Success)
  {
    seconds_from_vsync_to_photons_ = secondsToPhotons;
  }
  hmd_timing_props_cached_ = true;
}

bool OpenVRPresenter::AcquirePredictedPose(vr::HmdMatrix34_t &poseOut)
{
  if (!vr::VRSystem())
    return false;
  if (vr::VRCompositor() && !vr::VRCompositor()->CanRenderScene())
    return false;

  float secondsSinceLastVsync = 0.0f;
  uint64_t frameCounter = 0;
  const int kMaxAttempts = 500;
  int attempts = 0;
  bool haveNewFrame = false;
  while (attempts++ < kMaxAttempts)
  {
    if (!vr::VRSystem()->GetTimeSinceLastVsync(&secondsSinceLastVsync, &frameCounter))
      return false;
    if (!have_last_vsync_frame_counter_ || frameCounter != last_vsync_frame_counter_)
    {
      haveNewFrame = true;
      break;
    }
    std::this_thread::yield();
  }
  if (!haveNewFrame)
    return false;

  if (have_last_vsync_frame_counter_ && frameCounter > last_vsync_frame_counter_ + 1)
  {
    frames_skipped_debug_++;
  }

  last_vsync_frame_counter_ = frameCounter;
  have_last_vsync_frame_counter_ = true;

  float secondsSinceLastVsyncNow = 0.0f;
  uint64_t dummyCounter = 0;
  if (!vr::VRSystem()->GetTimeSinceLastVsync(&secondsSinceLastVsyncNow, &dummyCounter))
    return false;

  if (!hmd_timing_props_cached_)
  {
    UpdateHMDTimingProperties();
  }
  const float displayHz = (display_frequency_hz_ > 0.0f) ? display_frequency_hz_ : 90.0f;
  const double frameDur = 1.0 / static_cast<double>(displayHz);
  double predictedSeconds = frameDur - static_cast<double>(secondsSinceLastVsyncNow);
  predictedSeconds += static_cast<double>(seconds_from_vsync_to_photons_);
  if (predictedSeconds < 0.0)
    predictedSeconds = 0.0;

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
  vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, static_cast<float>(predictedSeconds), poses, vr::k_unMaxTrackedDeviceCount);
  const vr::TrackedDevicePose_t &hmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd];
  if (!hmdPose.bPoseIsValid)
    return false;
  poseOut = hmdPose.mDeviceToAbsoluteTracking;
  return true;
}

void OpenVRPresenter::SetStereoPanorama(bool enable)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!overlay_created_)
    return;
  // Mirror old pipeline: StereoPanorama true, Panorama false when enabled
  vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_Panorama, enable ? false : true);
  vr::VROverlay()->SetOverlayFlag(overlay_handle_, vr::VROverlayFlags_StereoPanorama, enable);
}

void OpenVRPresenter::SetFOVHalfRadians(float fovHalfRadians)
{
  std::lock_guard<std::mutex> lock(mtx_);
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
  row_major float4x4 lookRotation; // view rotation
  float halfFOVInRadians;
  float applyRotation; // 0=no, 1=yes
  float2 pad;
}

static const float PI = 3.141592;
static const float HALF_PI = 0.5 * PI;
static const float QUARTER_PI = 0.25 * PI;

struct PSIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
float4 main(PSIn i) : SV_Target {
  float2 uv = i.uv;

  // Match original GLSL shader's additional V flip before normalization
  float2 xy_flipped = float2(uv.x, 1.0 - uv.y);
  float2 xy_normalized = 2.0 * xy_flipped - 1.0;
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

  // Sample from SBS source: left half for left eye, right half for right eye
  float sampledU = eyeUV.x * 0.5 + (renderTopHalf ? 0.0 : 0.5);
  float sampledV = 1.0 - eyeUV.y;
  return srcTex.Sample(samp0, float2(sampledU, sampledV));
}
)HLSL";

bool OpenVRPresenter::EnsureShaderPipeline(UINT outWidth, UINT outHeight)
{
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
    if (shared_legacy_handle_)
    {
      CloseHandle(shared_legacy_handle_);
      shared_legacy_handle_ = nullptr;
    }
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

  if (present_count <= 3 || present_count % 120 == 0)
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

  // Always copy/resolve into our own SRV-capable single-sample texture for safe 120 Hz sampling
  bool needNewStage = (!last_source_tex_) ||
                      (copy_desc_.Width != srcDesc.Width) ||
                      (copy_desc_.Height != srcDesc.Height) ||
                      (copy_desc_.Format != srcDesc.Format);
  if (needNewStage)
  {
    last_source_tex_.Reset();
    last_source_srv_.Reset();
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
    HRESULT crt = device_->CreateTexture2D(&copy_desc_, nullptr, last_source_tex_.GetAddressOf());
    if (FAILED(crt))
    {
      std::cerr << "[OpenVR] ERROR: Failed to (re)create stage texture, hr=0x" << std::hex << crt << std::dec << "\n";
    }
    else
    {
      HRESULT srvHr = device_->CreateShaderResourceView(last_source_tex_.Get(), nullptr, last_source_srv_.GetAddressOf());
      if (FAILED(srvHr))
      {
        std::cerr << "[OpenVR] ERROR: Failed to create SRV for stage texture, hr=0x" << std::hex << srvHr << std::dec << "\n";
        last_source_srv_.Reset();
      }
    }
  }

  if (last_source_tex_)
  {
    if (srcDesc.SampleDesc.Count > 1)
    {
      context_->ResolveSubresource(last_source_tex_.Get(), 0, sharedTex.Get(), 0, srcDesc.Format);
    }
    else
    {
      context_->CopyResource(last_source_tex_.Get(), sharedTex.Get());
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
    // No-op
  }
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
  StopRenderLoop();
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
  if (shared_legacy_handle_)
  {
    CloseHandle(shared_legacy_handle_);
  }
  shared_legacy_handle_ = nullptr;
  ZeroMemory(&shared_legacy_desc_, sizeof(shared_legacy_desc_));
  last_submitted_texture_.Reset();
  copy_tex_.Reset();
  copy_srv_.Reset();
  ZeroMemory(&copy_desc_, sizeof(copy_desc_));

  context_.Reset();
  device_.Reset();
}

void OpenVRPresenter::StartRenderLoop()
{
  if (render_running_.load()) return;
  render_running_.store(true);
  render_thread_ = std::thread(&OpenVRPresenter::RenderLoop, this);
}

void OpenVRPresenter::StopRenderLoop()
{
  if (!render_running_.load()) return;
  render_running_.store(false);
  if (render_thread_.joinable())
  {
    render_thread_.join();
  }
}

void OpenVRPresenter::RenderLoop()
{
  using clock = std::chrono::steady_clock;
  auto first_ts = clock::now();
  auto last_ts = first_ts;
  auto last_report_ts = first_ts;
  int frame_count = 0;
  double worst_frame_ms = 0.0;
  // Raise priority to reduce scheduling jitter
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  while (render_running_.load())
  {
    vr::HmdMatrix34_t predictedPose = {};
    bool havePredictedPose = AcquirePredictedPose(predictedPose);
    if (!havePredictedPose)
    {
      std::this_thread::yield();
      continue;
    }

    bool rendered = false;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      bool canRender = device_ && context_ && openvr_initialized_ && overlay_created_ && last_source_srv_;
      if (canRender)
      {
        if (EnsureShaderPipeline(static_cast<UINT>(width_), static_cast<UINT>(height_)))
        {
          ID3D11ShaderResourceView *srvRaw = last_source_srv_.Get();
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
          context_->PSSetShaderResources(0, 1, &srvRaw);

          D3D11_MAPPED_SUBRESOURCE map = {};
          if (SUCCEEDED(context_->Map(cb_params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
          {
            float *dst = reinterpret_cast<float *>(map.pData);
            for (int r = 0; r < 4; ++r)
              for (int c = 0; c < 4; ++c)
                dst[r * 4 + c] = (r == c) ? 1.0f : 0.0f;
            bool appliedRotation = false;
            if (warp_follow_head_ && havePredictedPose)
            {
              BuildLookRotationMatrix(predictedPose, dst);
              appliedRotation = true;
            }
            dst[16] = fov_half_radians_;
            dst[17] = appliedRotation ? 1.0f : 0.0f;
            dst[18] = 0.0f;
            dst[19] = 0.0f;
            context_->Unmap(cb_params_.Get(), 0);
          }
          context_->VSSetConstantBuffers(0, 1, cb_params_.GetAddressOf());
          context_->PSSetConstantBuffers(0, 1, cb_params_.GetAddressOf());

          context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          context_->Draw(4, 0);
          context_->Flush();

          last_submitted_texture_ = shared_legacy_tex_;
          rendered = true;

          if (shared_legacy_handle_)
          {
            vr::Texture_t eyeTexture = {(void *)shared_legacy_handle_, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Auto};
            vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlay_handle_, &eyeTexture);
            if (overlayError != vr::VROverlayError_None)
            {
              std::cerr << "[OpenVR] ERROR: SetOverlayTexture failed: "
                        << vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError) << "\n";
            }
          }

          // Unbind to prevent leaks of references on the context
          ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
          context_->PSSetShaderResources(0, 1, nullSrv);
          ID3D11SamplerState *nullSampler[1] = {nullptr};
          context_->PSSetSamplers(0, 1, nullSampler);
          ID3D11RenderTargetView *nullRtv[1] = {nullptr};
          context_->OMSetRenderTargets(1, nullRtv, nullptr);
          context_->VSSetShader(nullptr, nullptr, 0);
          context_->PSSetShader(nullptr, nullptr, 0);
          context_->ClearState();

          // Drain overlay events to keep the queue from growing
          vr::VREvent_t evt;
          while (vr::VROverlay() && vr::VROverlay()->PollNextOverlayEvent(overlay_handle_, &evt, sizeof(evt)))
          {
            // No-op
          }
        }
      }
    }

    if (rendered)
    {
      auto now = clock::now();
      frame_count++;
      double delta_s = std::chrono::duration<double>(now - last_ts).count();
      last_ts = now;
      if (delta_s > 0.0)
      {
        double delta_ms = delta_s * 1000.0;
        if (delta_ms > worst_frame_ms) worst_frame_ms = delta_ms;
      }
      bool interval_report = (frame_count % 120) == 0;
      bool time_report = (std::chrono::duration<double>(now - last_report_ts).count() >= 5.0);
      if (frame_count == 1 || interval_report || time_report)
      {
        double total_s = std::chrono::duration<double>(now - first_ts).count();
        double avg_hz = total_s > 0.0 ? static_cast<double>(frame_count) / total_s : 0.0;
        double avg_ms = avg_hz > 0.0 ? 1000.0 / avg_hz : 0.0;
        double last_ms = delta_s > 0.0 ? delta_s * 1000.0 : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "[OpenVR] RenderLoop stats: total=" << frame_count
                  << " avg=" << avg_hz << "Hz (" << avg_ms << "ms)"
                  << " last=" << last_ms << "ms"
                  << " worst=" << worst_frame_ms << "ms"
                  << std::defaultfloat << "\n";
        last_report_ts = now;
      }
    }
  }
}
