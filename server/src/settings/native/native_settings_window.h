#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// The Direct2D settings window (appearance.settings_ui_backend = "d2d"). It adopts the
// HWND that settings_app.cpp creates, so the window class, single-instance activation,
// lingering and the worker that applies config writes stay shared with the WebView2
// frontend; only painting and input are handled here.
namespace native_settings
{
class SettingsWindowImpl;

class SettingsWindow
{
  public:
    struct Callbacks
    {
        // A client message, serialized exactly as the WebView2 page would post it.
        std::function<void(const std::string &message)> postToHost;
    };

    SettingsWindow(HWND hwnd, Callbacks callbacks, const std::string &initialPage);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    // Messages the window handles itself; returns true with `result` set when the host
    // must not process the message further.
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT &result);
    // A host message (configSnapshot, dictionaryResponse, ...), JSON.
    void OnHostMessage(const std::wstring &message);
    void SetLightTheme(bool light);
    void ShowPage(const std::string &pageId);
    // Drops hover and pressed states before the window is hidden, so the frame kept
    // for the next show does not flash a highlighted caption button.
    void ResetTransientState();

  private:
    std::unique_ptr<SettingsWindowImpl> impl_;
};
} // namespace native_settings
