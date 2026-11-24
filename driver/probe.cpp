#include <windows.h>
#include <winioctl.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>

#include "protocol.h"

using Microsoft::WRL::ComPtr;

static int endpoint() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(enumerator.GetAddressOf()));
    ComPtr<IMMDeviceCollection> devices;
    if (SUCCEEDED(result)) result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
    UINT count = 0;
    if (SUCCEEDED(result)) result = devices->GetCount(&count);
    if (FAILED(result)) return 2;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        if (FAILED(devices->Item(index, &device)) ||
            FAILED(device->OpenPropertyStore(STGM_READ, &properties))) continue;
        PROPVARIANT name{};
        const bool matches = SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal && std::wcscmp(name.pwszVal, NPU_MIC_ENDPOINT) == 0;
        PropVariantClear(&name);
        if (!matches) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) return 2;
        std::wprintf(L"%ls\n", id);
        CoTaskMemFree(id);
        return 0;
    }
    return 1;
}

static int tone() {
    const auto handle = CreateFileW(NPU_MIC_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Open driver failed (%lu).\n", GetLastError());
        return 2;
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> producer(handle, CloseHandle);
    const std::unique_ptr<void, decltype(&CloseHandle)> timer(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS),
        CloseHandle);
    LARGE_INTEGER due{};
    due.QuadPart = -100000;
    if (!timer || !SetWaitableTimer(timer.get(), &due, 10, nullptr, nullptr, FALSE)) return 2;
    std::array<short, NPU_MIC_PACKET_SAMPLES> packet{};
    for (size_t i = 0; i < packet.size(); ++i) {
        packet[i] = static_cast<short>(4000.0 * std::sin(6.283185307179586 * i / 96.0));
    }
    std::puts("Producing a 500 Hz test tone. Press Ctrl+C to stop.");
    while (WaitForSingleObject(timer.get(), INFINITE) == WAIT_OBJECT_0) {
        DWORD bytes;
        if (!DeviceIoControl(producer.get(), IOCTL_NPU_MIC_WRITE, packet.data(),
                             static_cast<DWORD>(sizeof(packet)), nullptr, 0, &bytes, nullptr)) {
            std::fprintf(stderr, "Write to driver failed (%lu).\n", GetLastError());
            return 2;
        }
    }
    return 2;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 1) return tone();
    if (argc != 2 || std::wcscmp(argv[1], L"--endpoint") != 0) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const int result = endpoint();
    CoUninitialize();
    return result;
}
