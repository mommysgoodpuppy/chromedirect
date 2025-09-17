#include "Client.h"
#include "OpenVRPresenter.h"
#include "D3DPresenter.h"
#include "Presenter.h"

#include <include/cef_app.h>
#include <include/cef_command_line.h>
#include <include/cef_sandbox_win.h>
#include <include/wrapper/cef_helpers.h>
#include <include/cef_render_process_handler.h>
#include <include/wrapper/cef_closure_task.h>
#include <include/wrapper/cef_library_loader.h>
#include "RendererBridge.h"

#include <windows.h>
#include <string>
#include <memory>
#include <csignal>
#include <atomic>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <cstdlib>

// Configuration (runtime via flags)
static bool g_enable_vr_mode = true; // --vr=false to use regular D3D presenter

// Global variables for cleanup
static std::atomic<bool> g_shutdown_requested{false};
static CefRefPtr<OffscreenClient> g_client;
static HWND g_main_window = nullptr;

// Console allocation for logging
static void AllocateConsole() {
  if (AllocConsole()) {
    freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);
    freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
    std::wcout.clear();
    std::wcerr.clear();
    std::wcin.clear();
    std::cout << "[CEF Demo] Console allocated for logging\n";
  }
}

// App to configure CEF switches for all processes
class SimpleApp : public CefApp {
 public:
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override {
    // Ensure OSR and GPU via ANGLE D3D11 are enabled everywhere
    if (!command_line->HasSwitch("off-screen-rendering-enabled"))
      command_line->AppendSwitch("off-screen-rendering-enabled");
    if (!command_line->HasSwitch("enable-gpu"))
      command_line->AppendSwitch("enable-gpu");
    if (!command_line->HasSwitch("use-angle"))
      command_line->AppendSwitchWithValue("use-angle", "d3d11");
    if (!command_line->HasSwitch("disable-gpu-sandbox"))
      command_line->AppendSwitch("disable-gpu-sandbox");
    // Unlock VSYNC/frame caps where possible (Chromium flags)
    if (!command_line->HasSwitch("disable-gpu-vsync"))
      command_line->AppendSwitch("disable-gpu-vsync");
    if (!command_line->HasSwitch("disable-frame-rate-limit"))
      command_line->AppendSwitch("disable-frame-rate-limit");
    // Smooth scheduling for OSR
    if (!command_line->HasSwitch("enable-begin-frame-scheduling"))
      command_line->AppendSwitch("enable-begin-frame-scheduling");
    if (!command_line->HasSwitch("remote-debugging-port"))
      command_line->AppendSwitchWithValue("remote-debugging-port", "9333");
  }
  // Provide a browser-process handler to pass flags to child processes.
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  // Provide a render-process handler to register test extensions when staged.
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

  IMPLEMENT_REFCOUNTING(SimpleApp);
 };

class SimpleBrowserHandler : public CefBrowserProcessHandler {
 public:
  void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override {
    // Propagate testing/dev switches into child (renderer) processes so the render handler can see them.
    CefRefPtr<CefCommandLine> gl = CefCommandLine::GetGlobalCommandLine();
    if (gl.get()) {
      // Ensure OSR/GPU flags also apply to child processes when a separate render app is used.
      if (!command_line->HasSwitch("off-screen-rendering-enabled"))
        command_line->AppendSwitch("off-screen-rendering-enabled");
      if (!command_line->HasSwitch("enable-gpu"))
        command_line->AppendSwitch("enable-gpu");
      if (!command_line->HasSwitch("use-angle"))
        command_line->AppendSwitchWithValue("use-angle", "d3d11");
      if (!command_line->HasSwitch("disable-gpu-sandbox"))
        command_line->AppendSwitch("disable-gpu-sandbox");
      if (!command_line->HasSwitch("disable-gpu-vsync"))
        command_line->AppendSwitch("disable-gpu-vsync");
      if (!command_line->HasSwitch("disable-frame-rate-limit"))
        command_line->AppendSwitch("disable-frame-rate-limit");
      if (!command_line->HasSwitch("enable-begin-frame-scheduling"))
        command_line->AppendSwitch("enable-begin-frame-scheduling");

      if (gl->HasSwitch("v8-ext-stage") && !command_line->HasSwitch("v8-ext-stage")) {
        command_line->AppendSwitchWithValue("v8-ext-stage", gl->GetSwitchValue("v8-ext-stage"));
      }
      if (gl->HasSwitch("disable-iwer-extension") && !command_line->HasSwitch("disable-iwer-extension")) {
        command_line->AppendSwitch("disable-iwer-extension");
      }
      if (gl->HasSwitch("v8-ping") && !command_line->HasSwitch("v8-ping")) {
        command_line->AppendSwitchWithValue("v8-ping", gl->GetSwitchValue("v8-ping"));
      }
      if (gl->HasSwitch("v8-post-vr") && !command_line->HasSwitch("v8-post-vr")) {
        command_line->AppendSwitchWithValue("v8-post-vr", gl->GetSwitchValue("v8-post-vr"));
      }
      if (gl->HasSwitch("enable-iwer-bridge") && !command_line->HasSwitch("enable-iwer-bridge")) {
        command_line->AppendSwitch("enable-iwer-bridge");
      }
    }
  }
  IMPLEMENT_REFCOUNTING(SimpleBrowserHandler);
};

// Return a static instance
CefRefPtr<CefBrowserProcessHandler> SimpleApp::GetBrowserProcessHandler() {
  static CefRefPtr<SimpleBrowserHandler> s_handler = new SimpleBrowserHandler();
  return s_handler;
}
// Forward declare handler singletons and select via flags at runtime.
namespace {
  CefRefPtr<CefRenderProcessHandler> g_minimal_handler; // created below
  CefRefPtr<CefRenderProcessHandler> g_iwer_handler;    // RendererBridge
}
CefRefPtr<CefRenderProcessHandler> SimpleApp::GetRenderProcessHandler() {
  CefRefPtr<CefCommandLine> gl = CefCommandLine::GetGlobalCommandLine();
  if (gl.get() && gl->HasSwitch("enable-iwer-bridge")) {
    if (!g_iwer_handler.get()) g_iwer_handler = new RendererBridge();
    return g_iwer_handler;
  }
  if (gl.get() && gl->HasSwitch("v8-ext-stage")) {
    return g_minimal_handler; // initialized later
  }
  return nullptr;
}

// Signal handler for Ctrl+C
static void SignalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_shutdown_requested.store(true);
    if (g_main_window) {
      PostMessage(g_main_window, WM_CLOSE, 0, 0);
    }
  }
}

// Console Ctrl handler for Windows
static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_shutdown_requested.store(true);
      if (g_main_window) {
        PostMessage(g_main_window, WM_CLOSE, 0, 0);
      }
      return TRUE;
    default:
      return FALSE;
  }
}

// Simple Win32 window to host our D3D11 swapchain
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_SIZE: {
      return 0;
    }
    case WM_CLOSE: {
      g_shutdown_requested.store(true);
      // Close browser first
      if (g_client && g_client->GetBrowser()) {
        g_client->GetBrowser()->GetHost()->CloseBrowser(true);
      }
      return 0;
    }
    case WM_DESTROY: {
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProc(hWnd, message, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR    lpCmdLine,
                      _In_ int       nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);

  // CEF sub-process entry point. Must be called before creating any UI.
  CefMainArgs main_args(hInstance);

#if defined(CEF_USE_SANDBOX)
  CefScopedSandboxInfo scoped_sandbox;
  void* sandbox_info = scoped_sandbox.sandbox_info();
#else
  void* sandbox_info = nullptr;
#endif

  // Optionally enable a minimal render-process handler for incremental V8 testing.
  // The handler is always available, and will self-gate via --v8-ext-stage.
  CefRefPtr<CefCommandLine> pre_cmd = CefCommandLine::CreateCommandLine();
  pre_cmd->InitFromString(::GetCommandLineW());

  class MinimalRenderHandler : public CefRenderProcessHandler {
   public:
    MinimalRenderHandler() : stage_(0), ext_registered_(false) {}
    void OnWebKitInitialized() override {
      // Stage 2+: Register a trivial JS-only extension that defines window.cefExt.ping
      CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
      stage_ = 0;
      if (cmd.get() && cmd->HasSwitch("v8-ext-stage")) {
        stage_ = std::max(1, atoi(cmd->GetSwitchValue("v8-ext-stage").ToString().c_str()));
      }
      // Defer true CefRegisterExtension to stage >= 3 only. Stage 2 uses safe JS injection in OnContextCreated.
      if (stage_ >= 3 && !ext_registered_) {
        // Minimal ES5 global attach; no window/this usage.
        const char* kExtJS =
          "if (!cefExt) var cefExt = {};\n"
          "if (!cefExt.ping) cefExt.ping = function(x){ return (x||0)+1; };\n";
        const bool ok = CefRegisterExtension("v8/cef_ext_ping", kExtJS, nullptr);
        ext_registered_ = ok;
      }
      // Stage 4+: Provide a minimal pose bridge API for IWER.
      if (stage_ >= 4) {
        const char* kPoseJS =
          "if (!iwerBridge) var iwerBridge = {};\n"
          "if (!iwerBridge.applyPose) iwerBridge.applyPose = function(s){\n"
          "  try {\n"
          "    var g = (typeof window !== 'undefined') ? window : this;\n"
          "    var d = g && g.xrDevice ? g.xrDevice : null;\n"
          "    if (!d) return false;\n"
          "    if (s && s.hmd) {\n"
          "      if (s.hmd.pos && d.position && d.position.set) d.position.set(s.hmd.pos[0], s.hmd.pos[1], s.hmd.pos[2]);\n"
          "      if (s.hmd.quat && d.quaternion && d.quaternion.set) d.quaternion.set(s.hmd.quat[0], s.hmd.quat[1], s.hmd.quat[2], s.hmd.quat[3]);\n"
          "    }\n"
          "    if (d.controllers) {\n"
          "      var L=d.controllers['left'], R=d.controllers['right'];\n"
          "      if (L && s && s.left) {\n"
          "        if (s.left.pos && L.position && L.position.set) L.position.set(s.left.pos[0], s.left.pos[1], s.left.pos[2]);\n"
          "        if (s.left.quat && L.quaternion && L.quaternion.set) L.quaternion.set(s.left.quat[0], s.left.quat[1], s.left.quat[2], s.left.quat[3]);\n"
          "      }\n"
          "      if (R && s && s.right) {\n"
          "        if (s.right.pos && R.position && R.position.set) R.position.set(s.right.pos[0], s.right.pos[1], s.right.pos[2]);\n"
          "        if (s.right.quat && R.quaternion && R.quaternion.set) R.quaternion.set(s.right.quat[0], s.right.quat[1], s.right.quat[2], s.right.quat[3]);\n"
          "      }\n"
          "    }\n"
          "    return true;\n"
          "  } catch(e){ return false; }\n"
          "};\n";
        CefRegisterExtension("v8/iwer_pose", kPoseJS, nullptr);
      }
    }
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
      // Post a simple log to the browser process to prove renderer is alive (only if staged).
      if (!frame.get()) return;

      // Stage info and optional context interaction.
      CefRefPtr<CefCommandLine> cmd = CefCommandLine::GetGlobalCommandLine();
      int stage = stage_;
      if (stage == 0 && cmd.get() && cmd->HasSwitch("v8-ext-stage")) {
        stage = std::max(1, atoi(cmd->GetSwitchValue("v8-ext-stage").ToString().c_str()));
      }
      if (stage > 0) {
        CefRefPtr<CefProcessMessage> pm0 = CefProcessMessage::Create("RB_LOG");
        pm0->GetArgumentList()->SetString(0, "ext-stage1: OnContextCreated");
        frame->SendProcessMessage(PID_BROWSER, pm0);
      }
      // Report stage and ext_registered_ to browser only if stage>0
      if (stage > 0) {
        CefRefPtr<CefProcessMessage> pm = CefProcessMessage::Create("RB_LOG");
        std::string info = std::string("ext-stage info: stage=") + std::to_string(stage) +
                           " main=" + (frame->IsMain()?"1":"0") +
                           " ext_registered=" + (ext_registered_?"1":"0");
        pm->GetArgumentList()->SetString(0, info);
        frame->SendProcessMessage(PID_BROWSER, pm);
      }
      // Do not modify the context at stage 3; rely solely on CefRegisterExtension.
    }
    IMPLEMENT_REFCOUNTING(MinimalRenderHandler);
   private:
    int stage_;
    bool ext_registered_;
  };
  // Initialize the minimal handler instance so SimpleApp can return it when staged.
  if (!g_minimal_handler.get()) g_minimal_handler = new MinimalRenderHandler();
  // Use a single app implementation for all processes.
  CefRefPtr<SimpleApp> app = new SimpleApp();
  const int exit_code = CefExecuteProcess(main_args, app, sandbox_info);
  if (exit_code >= 0) {
    // The sub-process has completed, so return here.
    // DON'T allocate console for sub-processes to avoid multiple windows
    return exit_code;
  }
  
  // Only allocate console for the main process
  AllocateConsole();
  std::cout << "[CEF Demo] Starting CEF application...\n";
  std::cout << "[CEF Demo] Main process continuing...\n";

  // Parse command line for runtime options
  CefRefPtr<CefCommandLine> app_cmd = CefCommandLine::CreateCommandLine();
  app_cmd->InitFromString(::GetCommandLineW());

  // Setup signal handlers
  std::cout << "[CEF Demo] Setting up signal handlers...\n";
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  // Window / headless setup and runtime options
  int width = 4000;   // overlay/output width
  int height = 2000;  // overlay/output height
  int cef_width = -1;  // browser/input width (defaults to overlay width)
  int cef_height = -1; // browser/input height (can default to half for panorama)
  float scale_m = 1.0f;
  std::string overlay_key = "cef.web.overlay";
  std::string start_url = "https://www.google.com";
  int target_fps = 120; // default higher than 60 to unlock
  bool stereo_panorama_flag = true; // default to stereo panorama
  bool shader_panorama = false;
  float fov_deg = 90.0f; // total FOV; half used in shader
  bool warp_follow_head = false;
  bool desktop_test_mode = false;

  if (app_cmd->HasSwitch("vr")) {
    const std::string v = app_cmd->GetSwitchValue("vr");
    if (!v.empty()) {
      g_enable_vr_mode = !(v == "0" || v == "false" || v == "no");
    }
  }
  if (app_cmd->HasSwitch("desktop-test")) {
    desktop_test_mode = true;
    g_enable_vr_mode = false;
    width = 1600;
    height = 900;
    target_fps = 60;
    stereo_panorama_flag = false;
    shader_panorama = false;
    std::cout << "[CEF Demo] Desktop test mode enabled (forcing D3D presenter)\n";
  }
  if (app_cmd->HasSwitch("width")) {
    width = std::max(64, atoi(app_cmd->GetSwitchValue("width").ToString().c_str()));
  }
  if (app_cmd->HasSwitch("height")) {
    height = std::max(64, atoi(app_cmd->GetSwitchValue("height").ToString().c_str()));
  }
  if (app_cmd->HasSwitch("cef-width")) {
    cef_width = std::max(64, atoi(app_cmd->GetSwitchValue("cef-width").ToString().c_str()));
  }
  if (app_cmd->HasSwitch("cef-height")) {
    cef_height = std::max(64, atoi(app_cmd->GetSwitchValue("cef-height").ToString().c_str()));
  }
  if (app_cmd->HasSwitch("scale")) {
    scale_m = std::max(0.01f, static_cast<float>(atof(app_cmd->GetSwitchValue("scale").ToString().c_str())));
  }
  if (app_cmd->HasSwitch("overlay-key")) {
    overlay_key = app_cmd->GetSwitchValue("overlay-key").ToString();
  }
  if (app_cmd->HasSwitch("url")) {
    start_url = app_cmd->GetSwitchValue("url").ToString();
  }
  if (app_cmd->HasSwitch("fps")) {
    target_fps = std::max(1, atoi(app_cmd->GetSwitchValue("fps").ToString().c_str()));
  }
  if (desktop_test_mode) {
    std::cout << "[CEF Demo] Desktop test defaults -> "
              << width << "x" << height << " @ " << target_fps << " FPS" << std::endl;
  }
  if (app_cmd->HasSwitch("overlay-stereo-panorama")) {
    const std::string v = app_cmd->GetSwitchValue("overlay-stereo-panorama");
    stereo_panorama_flag = (v.empty() || v == "1" || v == "true");
  }
  if (app_cmd->HasSwitch("shader-panorama")) {
    const std::string v = app_cmd->GetSwitchValue("shader-panorama");
    shader_panorama = (v.empty() || v == "1" || v == "true");
  }
  if (app_cmd->HasSwitch("fov-deg")) {
    fov_deg = std::max(1.0f, static_cast<float>(atof(app_cmd->GetSwitchValue("fov-deg").ToString().c_str())));
  }
  if (app_cmd->HasSwitch("warp-follow-head")) {
    const std::string v = app_cmd->GetSwitchValue("warp-follow-head");
    warp_follow_head = (v.empty() || v == "1" || v == "true");
  }
  HWND hWnd = nullptr;
  if (!g_enable_vr_mode) {
    std::cout << "[CEF Demo] Creating window...\n";
    const wchar_t kClassName[] = L"CEFOffscreenDemo";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = kClassName;
    RegisterClassExW(&wcex);

    hWnd = CreateWindowExW(0, kClassName, L"CEF Offscreen Accelerated Paint", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, 0, width, height, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
      std::cerr << "[CEF Demo] ERROR: Failed to create window!\n";
      return -1;
    }
    g_main_window = hWnd;
    std::cout << "[CEF Demo] Window created successfully\n";
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
  } else {
    std::cout << "[CEF Demo] VR mode: running headless (no Win32 window)\n";
  }

  // If not explicitly specified, pick CEF input size based on overlay/shader settings
  if (cef_width < 0) {
    if (g_enable_vr_mode) {
      cef_width = width / 2;
      //cef_width = shader_panorama ? std::max(64, width * 2) : width;
    } else {
      cef_width = width;
    }
  };
  if (cef_height < 0) {
    if (g_enable_vr_mode) {
      //cef_height = height
      cef_height = shader_panorama ? std::max(64, height / 4) : height;
    } else {
      cef_height = height;
    }
  };

  // CEF settings
  std::cout << "[CEF Demo] Configuring CEF settings...\n";
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true; // disable GPU sandbox path conflicts
  settings.log_severity = LOGSEVERITY_VERBOSE;
  settings.external_message_pump = false; // use CEF's built-in message loop
  settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  // Enable DevTools discovery for OSR targets
  settings.remote_debugging_port = 9333;
  
  // Set explicit paths relative to the executable directory so the app runs from any build folder
  wchar_t exePathW[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
  std::wstring exePath(exePathW);
  size_t slash = exePath.find_last_of(L"/\\");
  std::wstring binDirW = (slash == std::wstring::npos) ? L"." : exePath.substr(0, slash);
  CefString(&settings.resources_dir_path).FromWString(binDirW);
  CefString(&settings.locales_dir_path).FromWString(binDirW + L"/locales");
  CefString(&settings.cache_path).FromWString(binDirW + L"/cache");
  CefString(&settings.root_cache_path).FromWString(binDirW + L"/cache");
  CefString(&settings.browser_subprocess_path).FromWString(binDirW + L"/chromedirect_demo.exe");
  
  // Enable detailed logging to file
  CefString(&settings.log_file).FromWString(binDirW + L"/cef_detailed.log");
  // Force GPU/ANGLE D3D11 switches are applied in SimpleApp::OnBeforeCommandLineProcessing

  // Initialize CEF before creating presenters
  std::cout << "[CEF Demo] Initializing CEF...\n";
  if (!CefInitialize(main_args, settings, app, sandbox_info)) {
    std::cerr << "[CEF Demo] ERROR: CEF initialization failed!\n";
    std::cerr << "[CEF Demo] Check cef_detailed.log for details\n";
    return -1;
  }
  std::cout << "[CEF Demo] CEF initialized successfully\n";

  // Presenter (VR or regular D3D)
  std::shared_ptr<Presenter> presenter;
  if (g_enable_vr_mode) {
    std::cout << "[CEF Demo] Initializing OpenVR presenter...\n";
    std::shared_ptr<OpenVRPresenter> vr_presenter = std::make_shared<OpenVRPresenter>();
    if (!vr_presenter->Initialize(overlay_key.c_str(), width, height, scale_m)) {
      std::cerr << "[CEF Demo] ERROR: OpenVR presenter initialization failed!\n";
      CefShutdown();
      return -1;
    }
    std::cout << "[CEF Demo] OpenVR presenter initialized successfully\n";
    if (stereo_panorama_flag) {
      vr_presenter->SetStereoPanorama(true);
      std::cout << "[CEF Demo] Overlay flag: StereoPanorama enabled\n";
    }
    if (shader_panorama) {
      float fov_half_rad = (fov_deg * 0.5f) * 3.1415926535f / 180.0f;
      vr_presenter->ConfigurePanoramaShader(true, fov_half_rad);
      std::cout << "[CEF Demo] Shader panorama enabled (FOV half rad=" << fov_half_rad << ")\n";
      vr_presenter->SetWarpFollowHead(warp_follow_head);
      if (warp_follow_head) std::cout << "[CEF Demo] Warp follows head yaw enabled\n";
    }
    // Alpha handling defaults for AR: premultiplied on, don't ignore texture alpha
    vr_presenter->SetPremultipliedAlpha(true);
    vr_presenter->SetIgnoreTextureAlpha(false);
    presenter = vr_presenter;
  } else {
    std::cout << "[CEF Demo] Initializing D3D presenter...\n";
    std::shared_ptr<D3DPresenter> d3d_presenter = std::make_shared<D3DPresenter>();
    if (!d3d_presenter->Initialize(hWnd, width, height, 1.0f)) {
      std::cerr << "[CEF Demo] ERROR: D3D presenter initialization failed!\n";
      CefShutdown();
      return -1;
    }
    std::cout << "[CEF Demo] D3D presenter initialized successfully\n";
    presenter = d3d_presenter;
  }

  // Create browser windowless
  CefWindowInfo wi;
  if (g_enable_vr_mode) {
    wi.SetAsWindowless(nullptr);
  } else {
    wi.SetAsWindowless(hWnd);
  }
  wi.windowless_rendering_enabled = true;
  wi.shared_texture_enabled = true;
  
  // Set the D3D device for CEF to match our presenter (if needed)
  if (presenter && presenter->GetDevice()) {
    wi.shared_texture_enabled = true;
    std::cout << "[CEF Demo] Using presenter's D3D11 device with shared textures\n";
  }

  CefBrowserSettings bs;
  bs.windowless_frame_rate = target_fps;
  bs.background_color = CefColorSetARGB(0, 0, 0, 0);

  std::cout << "[CEF Demo] Creating browser client...\n";
  std::cout << "[CEF Demo] Browser/input size: " << cef_width << "x" << cef_height << ", overlay/output: " << width << "x" << height << "\n";
  g_client = new OffscreenClient(g_enable_vr_mode ? nullptr : hWnd, presenter, cef_width, cef_height, 1.0f, target_fps);
  CefRefPtr<CefClient> base_client = g_client;

  std::cout << "[CEF Demo] Creating browser with URL: " << start_url << "\n";
  if (!CefBrowserHost::CreateBrowser(wi, base_client, start_url, bs, nullptr, nullptr)) {
    std::cerr << "[CEF Demo] ERROR: Failed to create browser!\n";
    CefShutdown();
    return -1;
  }
  std::cout << "[CEF Demo] Browser creation initiated\n";

  // Message loop with shutdown check
  std::cout << "[CEF Demo] Starting message loop...\n";
  MSG msg;
  int frame_count = 0;
  while (!g_shutdown_requested.load()) {
    // Process Windows messages
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        std::cout << "[CEF Demo] WM_QUIT received\n";
        g_shutdown_requested.store(true);
        break;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    
    if (g_shutdown_requested.load()) {
      break;
    }
    
    // Process CEF work
    CefDoMessageLoopWork();
    
    // Log every 300 frames (roughly every 5 seconds at 60fps)
    if (++frame_count % 300 == 0) {
      std::cout << "[CEF Demo] Message loop running... (frame " << frame_count << ")\n";
    }
    
    Sleep(1); // Small delay to prevent 100% CPU usage
  }

  // Cleanup: Close browser and wait for it to close
  std::cout << "[CEF Demo] Shutting down...\n";
  if (g_client && g_client->GetBrowser()) {
    std::cout << "[CEF Demo] Closing browser...\n";
    g_client->GetBrowser()->GetHost()->CloseBrowser(true);
    
    // Wait for browser to close (with timeout)
    int timeout_ms = 5000;
    int elapsed_ms = 0;
    while (g_client->GetBrowser() && elapsed_ms < timeout_ms) {
      CefDoMessageLoopWork();
      Sleep(10);
      elapsed_ms += 10;
    }
    std::cout << "[CEF Demo] Browser closed\n";
  }

  // Reset global references
  g_client = nullptr;
  g_main_window = nullptr;
  
  std::cout << "[CEF Demo] Calling CefShutdown...\n";
  CefShutdown();
  std::cout << "[CEF Demo] Application terminated\n";
  return 0;
}
