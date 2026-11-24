#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::NpuBroadcast::implementation
{
    App::App()
    {
        InitializeComponent();
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            std::wcerr << L"NPU Broadcast UI error: " << e.Message().c_str() << L"\n";
        });
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        try {
            window = winrt::make<MainWindow>();
            window.Activate();
        } catch (winrt::hresult_error const& error) {
            std::wcerr << L"NPU Broadcast window failed: 0x" << std::hex << static_cast<uint32_t>(error.code())
                       << L" " << error.message().c_str() << L"\n";
            Exit();
        } catch (std::exception const& error) {
            std::cerr << "NPU Broadcast window failed: " << error.what() << "\n";
            Exit();
        }
    }
}
