#pragma once

#include <windows.h>
#include <dxcore.h>
#include <pdh.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class NpuUsage {
 public:
  explicit NpuUsage(unsigned int device_id);
  ~NpuUsage();
  NpuUsage(const NpuUsage &) = delete;
  NpuUsage &operator=(const NpuUsage &) = delete;

  std::optional<double> sample();

 private:
  bool is_npu(LUID luid);

  unsigned int device_id;
  std::wstring process_prefix;
  Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER counter = nullptr;
  std::unordered_map<ULONGLONG, bool> adapters;
  std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer;
};
