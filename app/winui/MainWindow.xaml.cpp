#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "app_context.h"
#include "tray.h"

#include <microsoft.ui.xaml.window.h>
#include <winioctl.h>
#include <shellapi.h>
#include "../../driver/protocol.h"
#include <cmath>
#include <cstdio>
#include <utility>

using namespace winrt;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::NpuBroadcast::implementation
{
namespace {
constexpr wchar_t settings_key[] = L"Software\\NPU Broadcast";

DWORD read_option(const wchar_t *name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_DWORD,
                        nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

std::wstring read_text(const wchar_t *name) {
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

bool write_setting(const wchar_t *name, DWORD type, const void *data, DWORD size) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, name, type, data, size) == ERROR_SUCCESS;
}

bool write_text(const wchar_t *name, const std::wstring &value) {
    return write_setting(name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

bool branded_microphone(const Endpoint &endpoint) {
    return endpoint.name == NPU_MIC_ENDPOINT ||
        (endpoint.cable && endpoint.name.rfind(std::wstring(NPU_MIC_ENDPOINT) + L" (", 0) == 0);
}
}

MainWindow::MainWindow()
{
    InitializeComponent();
    queue_ = DispatcherQueue::GetForCurrentThread();
    stop_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_) throw std::runtime_error("Could not create the audio stop event.");

    if (!npu::smoke_test()) RestoreSettings();
    auto bind_effect = [this](ToggleSwitch toggle, Grid panel, Slider slider, TextBlock value, std::atomic<int>& strength) {
        panel.IsHitTestVisible(toggle.IsOn());
        slider.IsEnabled(toggle.IsOn());
        toggle.Toggled([this, slider, panel](auto&& sender, auto&&) {
            const bool on = sender.template as<ToggleSwitch>().IsOn();
            panel.IsHitTestVisible(on);
            slider.IsEnabled(on);
            SaveOptions();
            ReconcileRoute();
        });
        strength = static_cast<int>(slider.Value());
        value.Text(std::to_wstring(strength.load()) + L"%");
        slider.ValueChanged([this, value, &strength](auto&&, auto&& args) {
            const int percent = static_cast<int>(args.NewValue());
            strength = percent;
            value.Text(std::to_wstring(percent) + L"%");
            SaveOptions();
        });
    };
    bind_effect(NoiseSwitch(), NoiseStrengthPanel(), NoiseStrength(), NoiseStrengthValue(), status_.noise_strength_percent);
    bind_effect(EchoSwitch(), EchoStrengthPanel(), EchoStrength(), EchoStrengthValue(), status_.echo_strength_percent);
    DeveloperConsole().Click([this](auto&&, auto&&) { UpdateDeveloperConsole(); SaveOptions(); });
    if (DeveloperConsole().IsChecked().Value()) UpdateDeveloperConsole();
    ListenBox().Click([this](auto&&, auto&&) { SaveOptions(); ReconcileRoute(); });
    KeepCapture().Click([this](auto&&, auto&&) { SaveOptions(); ReconcileRoute(); });
    Diagnostics().Expanding([this](auto&&, auto&&) { SaveOptions(); });
    Diagnostics().Collapsed([this](auto&&, auto&&) { SaveOptions(); });
    InputBox().SelectionChanged([this](auto&&, auto&&) {
        const auto selected = InputBox().SelectedIndex();
        if (updating_inputs_ || selected < 0 || selected > static_cast<int>(inputs_.size())) return;
        input_id_ = selected ? inputs_[selected - 1].id : L"";
        input_name_ = selected ? inputs_[selected - 1].name : L"";
        const bool saved_id = write_text(L"InputId", input_id_);
        if (!write_text(L"InputName", input_name_) || !saved_id) {
            ConsoleMessage().Text(L"Windows could not save the microphone selection.");
        }
        ReconcileRoute();
    });

    CableNameButton().Click([this](auto&&, auto&&) {
        const auto selected = cable_candidates_.size() == 1 ? 0 : CableSetupBox().SelectedIndex();
        if (selected < 0 || static_cast<size_t>(selected) >= cable_candidates_.size()) return;
        const auto &id = cable_candidates_[selected].id;
        wchar_t executable[32768];
        const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        if (!length || length == ARRAYSIZE(executable) || id.find(L'"') != std::wstring::npos) return;
        const auto command = L"--name-vb-cable \"" + id + L"\"";
        const auto result = ShellExecuteW(nullptr, L"runas", executable, command.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            StatusText().Text(L"Administrator approval is needed to name the cable.");
        }
    });

    activated_token_ = Activated({ this, &MainWindow::OnActivated });
    UpdateInputs();
    if (npu::smoke_test()) {
        std::cout << "Desktop controls initialized. Inputs " << inputs_.size()
                  << ", virtual microphone " << driver_ready_ << "\n";
        smoke_timer_ = DispatcherTimer();
        smoke_timer_.Interval(std::chrono::milliseconds(250));
        smoke_timer_.Tick([this](auto&&, auto&&) {
            smoke_timer_.Stop();
            Close();
            Application::Current().Exit();
        });
        smoke_timer_.Start();
    } else {
        timer_ = DispatcherTimer();
        timer_.Interval(std::chrono::milliseconds(150));
        timer_.Tick({ this, &MainWindow::OnTick });
        timer_.Start();
        ReconcileRoute();
    }
}

MainWindow::~MainWindow()
{
    StopRoute();
    if (worker_.joinable()) worker_.join();
    if (producer_ != INVALID_HANDLE_VALUE) CloseHandle(producer_);
    if (stop_event_) CloseHandle(stop_event_);
}

void MainWindow::OnActivated(IInspectable const&, WindowActivatedEventArgs const&)
{
    if (window_ready_) return;
    window_ready_ = true;
    UpdateTitleTheme();
    Surface().ActualThemeChanged([this](auto&&, auto&&) { UpdateTitleTheme(); });
    HWND hwnd = nullptr;
    Window window = *this;
    winrt::check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    npu_tray::attach(hwnd);
    npu_tray::install_window_hook(hwnd);
    npu_tray::set_handlers(
        [hwnd] { npu_tray::remove(); ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); },
        [hwnd] { PostMessageW(hwnd, WM_CLOSE, 0, 0); });

    AppWindow().Closing([this](auto&&, AppWindowClosingEventArgs const& args) {
        if (timer_) timer_.Stop();
        npu_tray::remove();
        if (worker_.joinable()) {
            args.Cancel(true);
            close_after_stop_ = true;
            StopRoute();
        } else if (producer_ != INVALID_HANDLE_VALUE) {
            CloseHandle(std::exchange(producer_, INVALID_HANDLE_VALUE));
        }
    });

    const double scale = GetDpiForWindow(hwnd) / 96.0;
    AppWindow().Resize({ static_cast<int32_t>(960 * scale), static_cast<int32_t>(720 * scale) });
    if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101))) {
        try {
            AppWindow().SetIcon(winrt::Microsoft::UI::IconId{ reinterpret_cast<uint64_t>(icon) });
        } catch (...) {}
    }
}

void MainWindow::UpdateTitleTheme()
{
    AppWindow().TitleBar().PreferredTheme(Surface().ActualTheme() == ElementTheme::Dark
        ? TitleBarTheme::Dark : TitleBarTheme::Light);
}

void MainWindow::UpdateDeveloperConsole()
{
    if (DeveloperConsole().IsChecked().Value()) {
        if (!AllocConsole()) {
            DeveloperConsole().IsChecked(false);
            ConsoleMessage().Text(L"Windows could not open a diagnostic console.");
            return;
        }
        SetConsoleTitleW(L"NPU Broadcast diagnostics");
        FILE *stream = nullptr;
        _wfreopen_s(&stream, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&stream, L"CONOUT$", L"w", stderr);
        std::cout.clear();
        std::cerr.clear();
    } else {
        FILE *stream = nullptr;
        _wfreopen_s(&stream, L"NUL", L"w", stdout);
        _wfreopen_s(&stream, L"NUL", L"w", stderr);
        FreeConsole();
    }
    ConsoleMessage().Text(L"Shows audio and model logs for this session.");
}

void MainWindow::OnNavSelectionChanged(IInspectable const&, NavigationViewSelectionChangedEventArgs const& args)
{
    const auto item = args.SelectedItem().try_as<NavigationViewItem>();
    if (!item) return;
    const auto tag = winrt::unbox_value_or<hstring>(item.Tag(), L"");
    RoutePage().Visibility(tag == L"route" ? Visibility::Visible : Visibility::Collapsed);
    DeveloperPage().Visibility(tag == L"developer" ? Visibility::Visible : Visibility::Collapsed);
    AboutPage().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::RestoreSettings()
{
    input_id_ = read_text(L"InputId");
    input_name_ = read_text(L"InputName");
    NoiseSwitch().IsOn(read_option(L"Noise", 0) == 1);
    EchoSwitch().IsOn(read_option(L"Echo", 0) == 1);
    NoiseStrength().Value(std::min<DWORD>(read_option(L"NoiseStrength", 100), 100));
    EchoStrength().Value(std::min<DWORD>(read_option(L"EchoStrength", 65), 100));
    ListenBox().IsChecked(read_option(L"Listen", 0) == 1);
    KeepCapture().IsChecked(read_option(L"KeepCapture", 0) == 1);
    DeveloperConsole().IsChecked(read_option(L"Console", 0) == 1);
    Diagnostics().IsExpanded(read_option(L"Diagnostics", 0) == 1);
}

void MainWindow::SaveOptions()
{
    const std::pair<const wchar_t *, DWORD> options[] = {
        {L"Noise", NoiseSwitch().IsOn()}, {L"Echo", EchoSwitch().IsOn()},
        {L"NoiseStrength", static_cast<DWORD>(NoiseStrength().Value())},
        {L"EchoStrength", static_cast<DWORD>(EchoStrength().Value())},
        {L"Listen", ListenBox().IsChecked().Value()}, {L"KeepCapture", KeepCapture().IsChecked().Value()},
        {L"Console", DeveloperConsole().IsChecked().Value()}, {L"Diagnostics", Diagnostics().IsExpanded()}
    };
    for (const auto &[name, value] : options) {
        if (!write_setting(name, REG_DWORD, &value, sizeof(value))) {
            ConsoleMessage().Text(L"Windows could not save your settings.");
            break;
        }
    }
}

void MainWindow::OnTick(IInspectable const&, IInspectable const&)
{
    npu_tray::check_activation();
    if (GetTickCount64() >= next_devices_) {
        next_devices_ = GetTickCount64() + 500;
        UpdateInputs();
        ReconcileRoute();
    }
    if (!npu_tray::active()) UpdateStatus();
}

Effect MainWindow::SelectedEffect()
{
    if (NoiseSwitch().IsOn()) return EchoSwitch().IsOn() ? Effect::combined : Effect::noise;
    return EchoSwitch().IsOn() ? Effect::room_echo : Effect::bypass;
}

void MainWindow::UpdateInputs()
{
    try {
        auto captures = active_endpoints(false);
        const auto playback = active_endpoints(true);
        Endpoint named_cable;
        Endpoint output;
        std::vector<Endpoint> candidates;
        bool native_available = false;
        bool ambiguous = false;
        for (const auto &capture : captures) {
            if (branded_microphone(capture)) {
                if (!capture.cable) {
                    native_available = true;
                } else if (!named_cable.id.empty()) {
                    ambiguous = true;
                } else {
                    named_cable = capture;
                    output = paired_vb_cable_output(capture, playback);
                }
            } else if (capture.cable && !paired_vb_cable_output(capture, playback).id.empty()) {
                candidates.push_back(capture);
            }
        }
        cable_output_ = ambiguous ? Endpoint{} : output;
        if (candidates != cable_candidates_) {
            cable_candidates_ = std::move(candidates);
            auto items = CableSetupBox().Items();
            items.Clear();
            for (const auto &candidate : cable_candidates_) items.Append(box_value(candidate.name.c_str()));
        }
        const auto setup_controls = cable_candidates_.empty() ? Visibility::Collapsed : Visibility::Visible;
        CableSetupBox().Visibility(cable_candidates_.size() > 1 ? Visibility::Visible : Visibility::Collapsed);
        CableNameButton().Visibility(setup_controls);
        CableNameButton().Content(box_value(cable_candidates_.size() == 1 ? L"Use existing VB-CABLE" : L"Name selected cable"));
        captures.erase(std::remove_if(captures.begin(), captures.end(), [](const Endpoint &endpoint) {
            return branded_microphone(endpoint) || endpoint.cable;
        }), captures.end());
        if (captures != inputs_ || InputBox().Items().Size() == 0) {
            inputs_ = std::move(captures);
            updating_inputs_ = true;
            auto items = InputBox().Items();
            items.Clear();
            items.Append(box_value(L"Windows default microphone"));
            int selected = input_id_.empty() ? 0 : -1;
            for (const auto &endpoint : inputs_) {
                items.Append(box_value(endpoint.name.c_str()));
                if (endpoint.id == input_id_) selected = static_cast<int>(items.Size()) - 1;
            }
            if (selected < 0) {
                items.Append(box_value((input_name_ + L" (unavailable)").c_str()));
                selected = static_cast<int>(items.Size()) - 1;
            }
            InputBox().SelectedIndex(selected);
            updating_inputs_ = false;
        }
        if (!cable_output_.id.empty()) {
            driver_ready_ = true;
            driver_drops_ = driver_underruns_ = 0;
            CableSetup().Visibility(Visibility::Collapsed);
            DestinationText().Text(L"In other apps, select " + named_cable.name + L" as your microphone.");
            consumer_active_ = !npu::smoke_test() && recording_endpoint_in_use(named_cable);
            if (producer_ != INVALID_HANDLE_VALUE && !worker_.joinable()) {
                CloseHandle(std::exchange(producer_, INVALID_HANDLE_VALUE));
            }
            return;
        }
        if (producer_ == INVALID_HANDLE_VALUE && !npu::smoke_test()) {
            producer_ = CreateFileW(NPU_MIC_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            driver_error_ = producer_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        }
        NpuMicStatus microphone{};
        DWORD bytes = 0;
        driver_ready_ = producer_ != INVALID_HANDLE_VALUE &&
            DeviceIoControl(producer_, IOCTL_NPU_MIC_STATUS, nullptr, 0, &microphone,
                            sizeof(microphone), &bytes, nullptr) && bytes == sizeof(microphone);
        if (!driver_ready_ && producer_ != INVALID_HANDLE_VALUE && !worker_.joinable()) {
            CloseHandle(std::exchange(producer_, INVALID_HANDLE_VALUE));
        }
        consumer_active_ = driver_ready_ && microphone.capture_streams > 0;
        driver_drops_ = microphone.dropped_samples;
        driver_underruns_ = microphone.underrun_samples;
        CableSetup().Visibility(driver_ready_ ? Visibility::Collapsed : Visibility::Visible);
        if (ambiguous) {
            DestinationText().Text(L"More than one VB-CABLE has the NPU Broadcast name. Keep that name on only one cable.");
        } else if (!named_cable.id.empty()) {
            DestinationText().Text(L"The named cable is missing its playback endpoint. Enable its CABLE Input device in Windows.");
        } else if (driver_error_ == ERROR_SHARING_VIOLATION || driver_error_ == ERROR_ACCESS_DENIED) {
            DestinationText().Text(L"Windows could not open the microphone driver. Check whether it is already in use.");
        } else if (!driver_ready_) {
            DestinationText().Text(cable_candidates_.empty()
                ? L"Install VB-CABLE to set up the NPU Broadcast microphone."
                : L"Use an installed cable to create the Microphone (NPU Broadcast) name.");
        } else if (!native_available) {
            DestinationText().Text(L"Waiting for Windows to publish Microphone (NPU Broadcast).");
        } else {
            DestinationText().Text(L"In other apps, select Microphone (NPU Broadcast) as your microphone.");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        consumer_active_ = false;
        driver_ready_ = false;
    }
}

void MainWindow::ReconcileRoute()
{
    if (close_after_stop_ || npu::smoke_test()) return;
    const Effect requested = SelectedEffect();
    const bool effects_enabled = requested != Effect::bypass;
    ListenBox().IsEnabled(effects_enabled);
    const bool wants_monitor = effects_enabled && ListenBox().IsChecked().Value();
    status_.requested_effect = requested;
    status_.listen = wants_monitor;
    const bool needed = effects_enabled || consumer_active_ || KeepCapture().IsChecked().Value();
    std::wstring selected = input_id_;
    try {
        if (selected.empty()) selected = default_endpoint_id(false);
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
    }
    const auto input = std::find_if(inputs_.begin(), inputs_.end(), [&](const auto &endpoint) {
        return endpoint.id == selected;
    });
    if (!driver_ready_ || input == inputs_.end() || !needed) {
        StopRoute();
        StatusText().Text(!driver_ready_ ? L"Virtual microphone unavailable" :
                          input == inputs_.end() ? L"Choose an available microphone" :
                          L"Idle");
        return;
    }
    if (worker_.joinable()) {
        if (running_input_ != input->id || running_output_ != cable_output_.id) StopRoute();
        return;
    }
    if (GetTickCount64() >= retry_after_) StartRoute(*input);
}

void MainWindow::StartRoute(const Endpoint &input)
{
    running_input_ = input.id;
    running_output_ = cable_output_.id;
    ResetEvent(stop_event_);
    status_.active = false;
    status_.effects_delivered = false;
    status_.effects_unavailable = false;
    status_.monitor_available = false;
    status_.missing = 0;
    status_.capture_gaps = 0;
    status_.discarded = 0;
    status_.input_level = 0;
    status_.output_level = 0;
    status_.inference_us = -1;
    status_.npu_device_id = 0;
    StatusText().Text(L"Opening microphone...");
    worker_ = std::thread([this, input, output = cable_output_, producer = cable_output_.id.empty() ? producer_ : nullptr] {
        std::wstring failure;
        try {
            run_window_route(input, output, producer, stop_event_, status_);
        } catch (const std::exception &error) {
            const std::string message = error.what();
            failure.assign(message.begin(), message.end());
            std::cerr << message << "\n";
        }
        queue_.TryEnqueue([this, failure] { FinishRun(failure); });
    });
}

void MainWindow::StopRoute()
{
    if (worker_.joinable() && !stopping_) {
        stopping_ = true;
        status_.listen = false;
        SetEvent(stop_event_);
    }
}

void MainWindow::FinishRun(std::wstring failure)
{
    if (worker_.joinable()) worker_.join();
    status_.active = false;
    status_.monitor_available = false;
    npu_usage_.reset();
    utilization_.reset();
    next_usage_sample_ = 0;
    const bool interrupted = std::exchange(stopping_, false);
    if (close_after_stop_) {
        Close();
        return;
    }
    if (!interrupted && !failure.empty()) {
        StatusText().Text(failure);
        retry_after_ = GetTickCount64() + 2000;
    }
    ReconcileRoute();
}

void MainWindow::UpdateMeter(int index, float peak)
{
    const auto track = index ? OutputTrack() : InputTrack();
    const auto meter = index ? OutputMeter() : InputMeter();
    const auto text = index ? OutputPeak() : InputPeak();
    const bool active = status_.active && !stopping_;
    const float db = 20.0f * std::log10((std::max)(peak, 0.001f));
    const float fraction = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    meter.Width(active ? track.ActualWidth() * fraction : 0.0);
    if (!active) {
        text.Text(L"Idle");
    } else if (db <= -60.0f) {
        text.Text(L"\u2264 -60 dBFS");
    } else {
        text.Text(std::to_wstring(std::lround(db)) + L" dBFS");
    }
}

void MainWindow::UpdateStatus()
{
    MonitorText().Text(status_.listen && status_.active && !status_.monitor_available
        ? L"Windows default playback device unavailable for monitoring." : L"");
    UpdateMeter(0, status_.input_level.load());
    UpdateMeter(1, status_.output_level.load());
    if (!status_.active || stopping_) {
        DropText().Text(L"No active audio stream.");
        return;
    }
    const auto elapsed = status_.inference_us.load();
    if (SelectedEffect() == Effect::bypass || status_.effects_unavailable || !status_.effects_delivered) {
        StatusText().Text(SelectedEffect() == Effect::bypass ? L"Effects off. Passing original voice." :
            status_.effects_unavailable ? L"Effects unavailable. Passing original voice." :
            L"Passing original voice while processed audio is unavailable.");
    } else if (elapsed < 0) {
        StatusText().Text(L"AMD NPU ready. Waiting for inference.");
    } else {
        const auto device_id = status_.npu_device_id.load();
        const auto now = GetTickCount64();
        if (device_id && now >= next_usage_sample_) {
            if (!npu_usage_) npu_usage_.emplace(device_id);
            utilization_ = npu_usage_->sample();
            next_usage_sample_ = now + 1000;
        }
        wchar_t text[96];
        if (utilization_) {
            swprintf_s(text, L"AMD NPU %.0f%% | Last inference %.1f ms", *utilization_, elapsed / 1000.0);
        } else {
            swprintf_s(text, L"AMD NPU active | Last inference %.1f ms", elapsed / 1000.0);
        }
        StatusText().Text(text);
    }
    DropText().Text(L"Gaps " + std::to_wstring(status_.capture_gaps.load()) + L" | Silence " +
        std::to_wstring(driver_underruns_ / 48) + L" ms | Discarded " +
        std::to_wstring(status_.discarded.load() + driver_drops_) + L" samples");
}
}
