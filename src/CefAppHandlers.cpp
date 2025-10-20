#include "CefAppHandlers.h"
#include "MinimalRenderHandler.h"
#include <include/cef_command_line.h>

// Global render process handler singleton
CefRefPtr<CefRenderProcessHandler> g_minimal_handler;

SimpleApp::SimpleApp() {}

void SimpleApp::OnBeforeCommandLineProcessing(const CefString &process_type,
                                              CefRefPtr<CefCommandLine> command_line)
{
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

CefRefPtr<CefBrowserProcessHandler> SimpleApp::GetBrowserProcessHandler()
{
  static CefRefPtr<SimpleBrowserHandler> s_handler = new SimpleBrowserHandler();
  return s_handler;
}

CefRefPtr<CefRenderProcessHandler> SimpleApp::GetRenderProcessHandler()
{
  // Always use the MinimalRenderHandler which provides the IWER bridge
  return g_minimal_handler; // initialized in main
}

void SimpleBrowserHandler::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line)
{
  // Propagate testing/dev switches into child (renderer) processes so the render handler can see them.
  CefRefPtr<CefCommandLine> gl = CefCommandLine::GetGlobalCommandLine();
  if (gl.get())
  {
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
  }
}
