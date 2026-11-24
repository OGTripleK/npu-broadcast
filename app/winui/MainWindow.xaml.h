#pragma once
#include "MainWindow.g.h"
#include "pch.h"

namespace winrt::NpuBroadcast::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void OnNavSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
    private:
        void OnActivated(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args);
        void OnTick(winrt::Windows::Foundation::IInspectable const& sender,
                    winrt::Windows::Foundation::IInspectable const& args);
        void UpdateInputs();
        void UpdateDeveloperConsole();
        void UpdateTitleTheme();
        void RestoreSettings();
        void SaveOptions();
        void ReconcileRoute();
        void StartRoute(const Endpoint &input);
        void StopRoute();
        void FinishRun(std::wstring failure);
        void UpdateStatus();
        void UpdateMeter(int index, float peak);
        Effect SelectedEffect();

        std::vector<Endpoint> inputs_;
        std::wstring input_id_;
        std::wstring input_name_;
        HANDLE producer_ = INVALID_HANDLE_VALUE;
        bool driver_ready_ = false;
        DWORD driver_error_ = ERROR_SUCCESS;
        DWORD driver_drops_ = 0;
        DWORD driver_underruns_ = 0;
        Endpoint cable_output_;
        std::vector<Endpoint> cable_candidates_;
        std::wstring running_input_;
        std::wstring running_output_;
        AudioStatus status_;
        HANDLE stop_event_ = nullptr;
        std::thread worker_;
        ULONGLONG next_devices_ = 0;
        ULONGLONG retry_after_ = 0;
        bool consumer_active_ = false;
        bool updating_inputs_ = false;
        bool stopping_ = false;
        bool window_ready_ = false;
        bool close_after_stop_ = false;
        std::optional<NpuUsage> npu_usage_;
        std::optional<double> utilization_;
        ULONGLONG next_usage_sample_ = 0;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer timer_{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer smoke_timer_{ nullptr };
        winrt::event_token activated_token_{};
    };
}

namespace winrt::NpuBroadcast::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
