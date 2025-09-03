#include "Client.h"
#include "OpenVRPresenter.h"
#include "D3DPresenter.h"
#include "Presenter.h"

#include <include/cef_app.h>
#include <include/cef_command_line.h>
#include <include/cef_sandbox_win.h>
#include <include/wrapper/cef_helpers.h>
#include <include/wrapper/cef_closure_task.h>
#include <include/wrapper/cef_library_loader.h>

#include <windows.h>
#include <string>
#include <memory>
#include <csignal>
#include <atomic>
#include <iostream>
#include <io.h>
#include <fcntl.h>

// Configuration
static constexpr bool ENABLE_VR_MODE = true; // Set to false to use regular D3D presenter

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
    // Smooth scheduling for OSR
    if (!command_line->HasSwitch("enable-begin-frame-scheduling"))
      command_line->AppendSwitch("enable-begin-frame-scheduling");
  }

  IMPLEMENT_REFCOUNTING(SimpleApp);
};

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

  const int exit_code = CefExecuteProcess(main_args, nullptr, sandbox_info);
  if (exit_code >= 0) {
    // The sub-process has completed, so return here.
    // DON'T allocate console for sub-processes to avoid multiple windows
    return exit_code;
  }
  
  // Only allocate console for the main process
  AllocateConsole();
  std::cout << "[CEF Demo] Starting CEF application...\n";
  std::cout << "[CEF Demo] Main process continuing...\n";

  // Setup signal handlers
  std::cout << "[CEF Demo] Setting up signal handlers...\n";
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  // Window / headless setup
  int width = 1280;
  int height = 720;
  HWND hWnd = nullptr;
  if constexpr (!ENABLE_VR_MODE) {
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

  // CEF settings
  std::cout << "[CEF Demo] Configuring CEF settings...\n";
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true; // disable GPU sandbox path conflicts
  settings.log_severity = LOGSEVERITY_VERBOSE;
  settings.external_message_pump = false; // use CEF's built-in message loop
  
  // Set explicit paths to help CEF find resources
  std::string bin_dir = "C:/GIT/chromedirect/build/bin";
  CefString(&settings.resources_dir_path).FromASCII(bin_dir.c_str());
  CefString(&settings.locales_dir_path).FromASCII((bin_dir + "/locales").c_str());
  CefString(&settings.cache_path).FromASCII((bin_dir + "/cache").c_str());
  CefString(&settings.root_cache_path).FromASCII((bin_dir + "/cache").c_str());
  CefString(&settings.browser_subprocess_path).FromASCII((bin_dir + "/chromedirect_demo.exe").c_str());
  
  // Enable detailed logging to file
  CefString(&settings.log_file).FromASCII((bin_dir + "/cef_detailed.log").c_str());
  settings.log_severity = LOGSEVERITY_VERBOSE;

  // Force GPU/ANGLE D3D11
  // (Using default helper path from CEF cmake integration)

  // Command line switches
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
  command_line->InitFromString(GetCommandLineW());
  command_line->AppendSwitch("enable-gpu");
  command_line->AppendSwitchWithValue("use-angle", "d3d11");
  command_line->AppendSwitch("disable-gpu-sandbox");
  command_line->AppendSwitch("off-screen-rendering-enabled");

  // Initialize CEF before creating presenters
  std::cout << "[CEF Demo] Initializing CEF...\n";
  CefRefPtr<CefApp> app = new SimpleApp();
  if (!CefInitialize(main_args, settings, app, sandbox_info)) {
    std::cerr << "[CEF Demo] ERROR: CEF initialization failed!\n";
    std::cerr << "[CEF Demo] Check cef_detailed.log for details\n";
    return -1;
  }
  std::cout << "[CEF Demo] CEF initialized successfully\n";

  // Presenter (VR or regular D3D)
  std::shared_ptr<Presenter> presenter;
  if constexpr (ENABLE_VR_MODE) {
    std::cout << "[CEF Demo] Initializing OpenVR presenter...\n";
    std::shared_ptr<OpenVRPresenter> vr_presenter = std::make_shared<OpenVRPresenter>();
    if (!vr_presenter->Initialize("cef.web.overlay", width, height, 1.0f)) {
      std::cerr << "[CEF Demo] ERROR: OpenVR presenter initialization failed!\n";
      CefShutdown();
      return -1;
    }
    std::cout << "[CEF Demo] OpenVR presenter initialized successfully\n";
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
  if constexpr (ENABLE_VR_MODE) {
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
  bs.windowless_frame_rate = 60;

  std::cout << "[CEF Demo] Creating browser client...\n";
  g_client = new OffscreenClient(ENABLE_VR_MODE ? nullptr : hWnd, presenter, width, height, 1.0f);
  CefRefPtr<CefClient> base_client = g_client;

  std::cout << "[CEF Demo] Creating browser with URL: https://www.google.com\n";
  if (!CefBrowserHost::CreateBrowser(wi, base_client, "https://www.google.com", bs, nullptr, nullptr)) {
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
