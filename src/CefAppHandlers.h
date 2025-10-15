#pragma once

#include <include/cef_app.h>
#include <include/cef_browser_process_handler.h>
#include <include/cef_render_process_handler.h>

// Forward declaration
class MinimalRenderHandler;

// SimpleApp configures CEF switches for all processes and provides handlers
class SimpleApp : public CefApp
{
public:
  SimpleApp();

  void OnBeforeCommandLineProcessing(const CefString &process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

  IMPLEMENT_REFCOUNTING(SimpleApp);
};

// SimpleBrowserHandler propagates flags to child processes
class SimpleBrowserHandler : public CefBrowserProcessHandler
{
public:
  void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override;

  IMPLEMENT_REFCOUNTING(SimpleBrowserHandler);
};

// Global render process handler singleton
extern CefRefPtr<CefRenderProcessHandler> g_minimal_handler;
