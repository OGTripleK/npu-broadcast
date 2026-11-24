#define NOMINMAX
#include "npu_usage.h"

#include <cmath>
#include <cwchar>
#include <pdhmsg.h>
#include <string_view>

using Microsoft::WRL::ComPtr;

NpuUsage::NpuUsage(unsigned int device_id)
    : device_id(device_id), process_prefix(L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_luid_") {
  if (!device_id || FAILED(DXCoreCreateAdapterFactory(factory.GetAddressOf()))) return;
  if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
  if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
    counter = nullptr;
  }
}

NpuUsage::~NpuUsage() {
  if (query) PdhCloseQuery(query);
}

bool NpuUsage::is_npu(LUID luid) {
  const auto key = (static_cast<ULONGLONG>(static_cast<DWORD>(luid.HighPart)) << 32) | luid.LowPart;
  const auto cached = adapters.find(key);
  if (cached != adapters.end()) return cached->second;

  ComPtr<IDXCoreAdapter> adapter;
  if (FAILED(factory->GetAdapterByLuid(luid, adapter.GetAddressOf()))) return false;
  DXCoreHardwareID hardware{};
  if (FAILED(adapter->GetProperty(DXCoreAdapterProperty::HardwareID, &hardware))) return false;
  const bool matches = hardware.vendorID == 0x1022 && hardware.deviceID == device_id;
  adapters.emplace(key, matches);
  return matches;
}

std::optional<double> NpuUsage::sample() {
  if (!counter || PdhCollectQueryData(query) != ERROR_SUCCESS) return {};
  DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
  DWORD count = 0;
  auto result = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.data());
  if (result == PDH_MORE_DATA) {
    buffer.resize((bytes + sizeof(buffer[0]) - 1) / sizeof(buffer[0]));
    bytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
    result = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.data());
  }
  if (result != ERROR_SUCCESS) return {};

  std::optional<double> busiest;
  for (DWORD index = 0; index < count; ++index) {
    const auto &item = buffer[index];
    if (!item.szName || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                         item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) continue;
    const std::wstring_view name(item.szName);
    if (name.substr(0, process_prefix.size()) != process_prefix) continue;

    unsigned int high = 0, low = 0;
    if (swscanf_s(item.szName + process_prefix.size(), L"0x%8x_0x%8x", &high, &low) != 2 ||
        !is_npu({static_cast<DWORD>(low), static_cast<LONG>(high)})) continue;
    const auto usage = item.FmtValue.doubleValue;
    if (std::isfinite(usage) && usage >= 0.0 && usage <= 100.0 && (!busiest || usage > *busiest)) busiest = usage;
  }
  return busiest;
}
