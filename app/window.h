#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

struct Endpoint {
  std::wstring id;
  std::wstring name;
  std::wstring interface_name;
  bool cable = false;
  bool operator==(const Endpoint &other) const { return id == other.id && name == other.name; }
};

enum class Effect { noise, room_echo, combined, bypass };

struct AudioStatus {
  std::atomic<int> noise_strength_percent{100};
  std::atomic<int> echo_strength_percent{100};
  std::atomic<bool> active{false};
  std::atomic<float> input_level{0};
  std::atomic<float> output_level{0};
  std::atomic<bool> listen{false};
  std::atomic<Effect> requested_effect{Effect::bypass};
  std::atomic<bool> effects_unavailable{false};
  std::atomic<bool> effects_delivered{false};
  std::atomic<bool> monitor_available{false};
  std::atomic<long long> inference_us{-1};
  std::atomic<unsigned int> npu_device_id{0};
  std::atomic<size_t> missing{0};
  std::atomic<size_t> capture_gaps{0};
  std::atomic<size_t> discarded{0};
};

std::vector<Endpoint> active_endpoints(bool render);
std::wstring default_endpoint_id(bool render);
bool extra_vb_cable(const Endpoint &endpoint);
Endpoint paired_vb_cable_output(const Endpoint &microphone, const std::vector<Endpoint> &playback);
bool recording_endpoint_in_use(const Endpoint &endpoint);
void run_window_route(const Endpoint &input, const Endpoint &cable_output, HANDLE producer,
                      HANDLE stop, AudioStatus &status);
int show_window(bool smoke_test = false);
