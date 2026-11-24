#include "pch.h"
#include "App.xaml.h"
#include "window.h"
#include "app_context.h"
#include "tray.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

namespace {

constexpr UINT tray_callback = WM_APP + 20;
constexpr UINT tray_open_id = 102;
constexpr UINT tray_exit_id = 104;
constexpr int app_icon_id = 101;

// Owns the notification area icon. The icon lives on a message-only window
// so it keeps working while the XAML window is hidden.
struct TrayHost {
    HANDLE activation = nullptr;
    HWND command_target = nullptr;
    HWND messenger = nullptr;
    bool in_tray = false;
    const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    std::function<void()> on_restore;
    std::function<void()> on_exit;

    NOTIFYICONDATAW icon_data() const {
        NOTIFYICONDATAW result{};
        result.cbSize = sizeof(result);
        result.hWnd = messenger;
        result.uID = 1;
        return result;
    }

    bool add_icon() {
        if (in_tray) return true;
        if (!messenger) return false;
        auto payload = icon_data();
        payload.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        payload.uCallbackMessage = tray_callback;
        payload.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(app_icon_id),
                                                      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                      GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!payload.hIcon) return false;
        wcscpy_s(payload.szTip, L"NPU Broadcast");
        if (!Shell_NotifyIconW(NIM_ADD, &payload)) return false;
        payload.uVersion = NOTIFYICON_VERSION_4;
        in_tray = Shell_NotifyIconW(NIM_SETVERSION, &payload) != FALSE;
        if (!in_tray) {
            auto cleanup = icon_data();
            Shell_NotifyIconW(NIM_DELETE, &cleanup);
        }
        return in_tray;
    }

    void remove_icon() {
        if (!in_tray) return;
        auto payload = icon_data();
        Shell_NotifyIconW(NIM_DELETE, &payload);
        in_tray = false;
    }

    void menu(POINT origin) {
        if (!messenger) return;
        auto menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, tray_open_id, L"Open NPU Broadcast");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, tray_exit_id, L"Exit");
        SetMenuDefaultItem(menu, tray_open_id, FALSE);
        if (command_target) SetForegroundWindow(command_target);
        const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                             origin.x, origin.y, 0, messenger, nullptr);
        DestroyMenu(menu);
        PostMessageW(messenger, WM_NULL, 0, 0);
        switch (selected) {
            case tray_open_id:
                if (on_restore) on_restore();
                break;
            case tray_exit_id:
                if (on_exit) on_exit();
                break;
            default: {
                auto payload = icon_data();
                Shell_NotifyIconW(NIM_SETFOCUS, &payload);
                break;
            }
        }
    }

    static LRESULT CALLBACK messenger_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* host = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (!host) return DefWindowProcW(handle, message, wparam, lparam);
        if (message == host->taskbar_created && host->in_tray) {
            host->in_tray = false;
            if (!host->add_icon()) host->remove_icon();
            return 0;
        }
        switch (message) {
            case tray_callback: {
                if (!host->in_tray) return 0;
                const auto event = LOWORD(lparam);
                if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) {
                    if (host->on_restore) host->on_restore();
                }
                if (event == WM_CONTEXTMENU) {
                    host->menu({ GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam) });
                }
                return 0;
            }
            default:
                return DefWindowProcW(handle, message, wparam, lparam);
        }
    }
};

TrayHost& tray() {
    static TrayHost host;
    return host;
}

// Runs on the XAML window. Minimizing hides the window into the tray.
LRESULT CALLBACK hook_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam,
                           UINT_PTR, DWORD_PTR) {
    switch (message) {
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_MINIMIZE) {
                if (tray().add_icon()) {
                    ShowWindow(handle, SW_HIDE);
                    return 0;
                }
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(handle, message, wparam, lparam);
}

}  // namespace

namespace npu_tray {

void attach(HWND command_target) {
    auto& host = tray();
    host.command_target = command_target;
    if (host.messenger) return;
    WNDCLASSW settings{};
    settings.lpfnWndProc = TrayHost::messenger_proc;
    settings.hInstance = GetModuleHandleW(nullptr);
    settings.lpszClassName = L"NpuBroadcastTrayHost";
    if (!RegisterClassW(&settings) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    host.messenger = CreateWindowExW(0, L"NpuBroadcastTrayHost", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                     nullptr, GetModuleHandleW(nullptr), nullptr);
    if (host.messenger) {
        SetWindowLongPtrW(host.messenger, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&host));
    }
}

HWND messenger() {
    return tray().messenger;
}

bool add() {
    return tray().add_icon();
}

void remove() {
    tray().remove_icon();
}

bool active() {
    return tray().in_tray;
}

void install_window_hook(HWND window) {
    if (!window) return;
    SetWindowSubclass(window, hook_proc, 1, 0);
}

void set_handlers(std::function<void()> on_restore, std::function<void()> on_exit) {
    auto& host = tray();
    host.on_restore = std::move(on_restore);
    host.on_exit = std::move(on_exit);
}

void check_activation() {
    auto &host = tray();
    if (host.activation && host.on_restore && WaitForSingleObject(host.activation, 0) == WAIT_OBJECT_0) {
        host.on_restore();
    }
}

}  // namespace npu_tray

int show_window(bool smoke_test) {
    const std::unique_ptr<void, decltype(&CloseHandle)> activation(smoke_test ? nullptr :
        CreateEventW(nullptr, FALSE, FALSE, L"Local\\NpuBroadcast.Activate"), CloseHandle);
    const HANDLE mutex = smoke_test ? nullptr : CreateMutexW(nullptr, FALSE, L"Local\\NpuBroadcast.Desktop");
    const DWORD mutex_result = GetLastError();
    const std::unique_ptr<void, decltype(&CloseHandle)> instance(mutex, CloseHandle);
    if (!smoke_test && (!activation || !instance)) return 1;
    if (!smoke_test && mutex_result == ERROR_ALREADY_EXISTS) {
        AllowSetForegroundWindow(ASFW_ANY);
        SetEvent(activation.get());
        return 0;
    }
    tray().activation = activation.get();
    npu::smoke_test() = smoke_test;

    // Per-monitor DPI comes from the manifest; keep the call for older loaders.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    npu_tray::attach(nullptr);

    if (smoke_test) {
        if (npu_tray::add()) {
            const NOTIFYICONIDENTIFIER icon{ sizeof(NOTIFYICONIDENTIFIER), npu_tray::messenger(), 1, {} };
            RECT icon_rect{};
            const bool found = SUCCEEDED(Shell_NotifyIconGetRect(&icon, &icon_rect));
            npu_tray::remove();
            std::cout << "Tray icon " << (found ? "verified in the shell" : "registered with the shell") << ".\n";
        } else {
            std::cout << "Notification area unavailable to this process.\n";
        }
    }

    // Unpackaged self-contained apps load the Windows App SDK runtime from
    // the exe folder; no bootstrap initialization is required.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
        return winrt::make<winrt::NpuBroadcast::implementation::App>();
    });
    return 0;
}
