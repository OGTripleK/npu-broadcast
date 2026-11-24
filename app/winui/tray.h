#pragma once

#include <windows.h>
#include <functional>

// Notification area support shared by the XAML window and the smoke test.
// The tray icon is owned by a message-only window on the UI thread so it
// keeps working while the XAML window is hidden.
namespace npu_tray {

// Creates the message-only window that owns the notification area icon.
// command_target receives restore/exit requests through the handlers
// registered with set_handlers.
void attach(HWND command_target);

HWND messenger();

bool add();
void remove();
bool active();

// Subclasses the XAML window so minimizing hides it into the tray.
void install_window_hook(HWND window);

void set_handlers(std::function<void()> on_restore, std::function<void()> on_exit);
void check_activation();

}  // namespace npu_tray
