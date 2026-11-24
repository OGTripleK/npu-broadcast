#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <winioctl.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "window.h"
#include "../driver/protocol.h"

extern "C" {
#include "rnnoise.h"
#include "rnn_winml.h"
}

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int samples_per_frame = 480;
constexpr UINT64 ticks_per_second = 10000000;
constexpr UINT64 processing_rate = 48000;
constexpr float pcm_scale = 32768.0f;
constexpr int echo_hop = 256;
constexpr int echo_frame_samples = echo_hop * 3;
HANDLE stop_signal = nullptr;

void check(HRESULT result, const char *operation) {
  if (SUCCEEDED(result)) return;
  std::ostringstream error;
  error << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ")";
  throw std::runtime_error(error.str());
}

struct ComApartment {
  explicit ComApartment(DWORD model = COINIT_APARTMENTTHREADED) {
    check(CoInitializeEx(nullptr, model), "CoInitializeEx");
  }
  ~ComApartment() { CoUninitialize(); }
};

struct Event {
  HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Event() {
    if (!handle) throw std::runtime_error("Could not create the audio event.");
  }
  ~Event() { CloseHandle(handle); }
};

BOOL WINAPI stop_on_console_signal(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  SetEvent(stop_signal);
  return TRUE;
}

struct ConsoleStop {
  Event event;
  ConsoleStop() {
    stop_signal = event.handle;
    if (!SetConsoleCtrlHandler(stop_on_console_signal, TRUE)) {
      throw std::runtime_error("Could not install the console stop handler.");
    }
  }
  ~ConsoleStop() { SetConsoleCtrlHandler(stop_on_console_signal, FALSE); }
};

ComPtr<IMMDeviceCollection> endpoints(IMMDeviceEnumerator *enumerator, EDataFlow direction) {
  ComPtr<IMMDeviceCollection> result;
  check(enumerator->EnumAudioEndpoints(direction, DEVICE_STATE_ACTIVE, &result), "EnumAudioEndpoints");
  return result;
}

ComPtr<IMMDeviceEnumerator> device_enumerator() {
  ComPtr<IMMDeviceEnumerator> result;
  check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(result.GetAddressOf())), "Create device enumerator");
  return result;
}

std::vector<Endpoint> describe_endpoints(IMMDeviceCollection *devices) {
  UINT count;
  check(devices->GetCount(&count), "GetCount");
  std::vector<Endpoint> result;
  result.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    ComPtr<IPropertyStore> properties;
    check(devices->Item(index, &device), "Item");
    check(device->OpenPropertyStore(STGM_READ, &properties), "OpenPropertyStore");
    PROPVARIANT name;
    PropVariantInit(&name);
    check(properties->GetValue(PKEY_Device_FriendlyName, &name), "GetValue");
    std::wstring friendly_name = name.vt == VT_LPWSTR ? name.pwszVal : L"(unnamed endpoint)";
    PropVariantClear(&name);
    PropVariantInit(&name);
    const std::wstring interface_name = SUCCEEDED(properties->GetValue(PKEY_DeviceInterface_FriendlyName, &name))
        && name.vt == VT_LPWSTR && name.pwszVal ? name.pwszVal : L"";
    const bool cable = interface_name == L"VB-Audio Virtual Cable" ||
        interface_name.rfind(L"VB-Audio Cable ", 0) == 0 ||
        interface_name.rfind(L"VB-Audio Virtual Cable ", 0) == 0 ||
        friendly_name.find(L"(VB-Audio Virtual Cable)") != std::wstring::npos ||
        friendly_name.find(L"(VB-Audio Cable ") != std::wstring::npos;
    PropVariantClear(&name);
    LPWSTR id = nullptr;
    check(device->GetId(&id), "Get device ID");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> endpoint_id(id, CoTaskMemFree);
    result.push_back({id, std::move(friendly_name), interface_name, cable});
  }
  return result;
}

void list_devices(IMMDeviceCollection *devices, const wchar_t *label) {
  const auto entries = describe_endpoints(devices);
  for (size_t index = 0; index < entries.size(); ++index) {
    std::wcout << label << L" " << index << L"  " << entries[index].name << L"\n";
  }
}

unsigned long number(const wchar_t *text) {
  size_t end = 0;
  const auto value = std::stoul(text, &end);
  if (text[end] != L'\0') throw std::invalid_argument("Use a numeric endpoint index or duration.");
  return value;
}

ComPtr<IMMDevice> device_at(IMMDeviceCollection *devices, UINT index) {
  ComPtr<IMMDevice> device;
  check(devices->Item(index, &device), "Open selected endpoint");
  return device;
}

WAVEFORMATEX processing_format() {
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = 1;
  format.nSamplesPerSec = static_cast<DWORD>(processing_rate);
  format.wBitsPerSample = 32;
  format.nBlockAlign = sizeof(float);
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  return format;
}

void report_period(IAudioClient *client, const char *endpoint) {
  ComPtr<IAudioClient3> modern;
  if (FAILED(client->QueryInterface(IID_PPV_ARGS(modern.GetAddressOf())))) return;
  const auto format = processing_format();
  UINT32 normal, fundamental, minimum, maximum;
  if (SUCCEEDED(modern->GetSharedModeEnginePeriod(&format, &normal, &fundamental, &minimum, &maximum))) {
    std::cout << endpoint << " shared engine periods at 48 kHz: default " << normal
              << ", minimum " << minimum << ", maximum " << maximum << " samples.\n";
  }
  WAVEFORMATEX *raw_mix = nullptr;
  if (FAILED(client->GetMixFormat(&raw_mix))) return;
  const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(raw_mix, CoTaskMemFree);
  std::cout << endpoint << " native mix " << mix->nSamplesPerSec << " Hz, " << mix->nChannels
            << " channels, " << mix->wBitsPerSample << " bits per sample\n";
  if (SUCCEEDED(modern->GetSharedModeEnginePeriod(mix.get(), &normal, &fundamental, &minimum, &maximum))) {
    std::cout << endpoint << " native engine minimum " << minimum << " samples, default " << normal << "\n";
  }
}

ComPtr<IAudioClient> audio_client(IMMDevice *device, const Event &event, bool loopback = false,
                                  DWORD extra_flags = 0) {
  ComPtr<IAudioClient> client;
  check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void **>(client.GetAddressOf())), "Activate IAudioClient");
  const auto format = processing_format();
  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) |
                      extra_flags;
  const HRESULT initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr);
  if (FAILED(initialized) && extra_flags) {
    client.Reset();
    return audio_client(device, event, loopback);
  }
  check(initialized, "Initialize audio endpoint");
  check(client->SetEventHandle(event.handle), "SetEventHandle");
  return client;
}

struct ProbeRecorder {
  ComPtr<IAudioCaptureClient> reader;
  std::ofstream stream;

  ProbeRecorder(IAudioClient *client, const std::filesystem::path &path)
      : stream(path, std::ios::binary | std::ios::trunc) {
    if (!stream) throw std::runtime_error("Could not open the probe capture file.");
    check(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(reader.GetAddressOf())),
          "Get probe capture service");
  }

  void drain() {
    UINT32 queued;
    check(reader->GetNextPacketSize(&queued), "Get probe packet size");
    while (queued) {
      BYTE *data;
      UINT32 samples;
      DWORD flags;
      UINT64 position;
      check(reader->GetBuffer(&data, &samples, &flags, nullptr, &position), "Get probe capture buffer");
      stream.write(reinterpret_cast<const char *>(&position), sizeof(position));
      stream.write(reinterpret_cast<const char *>(&samples), sizeof(samples));
      stream.write(reinterpret_cast<const char *>(&flags), sizeof(flags));
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        constexpr float silence = 0.0f;
        for (UINT32 index = 0; index < samples; ++index) {
          stream.write(reinterpret_cast<const char *>(&silence), sizeof(silence));
        }
      } else {
        stream.write(reinterpret_cast<const char *>(data), samples * sizeof(float));
      }
      check(reader->ReleaseBuffer(samples), "Release probe capture buffer");
      if (!stream) throw std::runtime_error("Could not write probe capture.");
      check(reader->GetNextPacketSize(&queued), "Get probe packet size");
    }
  }
};

void route_probe(IMMDevice *source, IMMDevice *sink, const std::filesystem::path &clip,
                 std::filesystem::path result) {
  const auto bytes = std::filesystem::file_size(clip);
  if (bytes % sizeof(short) || bytes < 48000 * sizeof(short) || bytes > 48000 * sizeof(short) * 8) {
    throw std::runtime_error("The probe needs 1–8 seconds of 48 kHz mono signed 16-bit PCM.");
  }
  std::vector<short> speech(static_cast<size_t>(bytes / sizeof(short)));
  std::ifstream file(clip, std::ios::binary);
  if (!file.read(reinterpret_cast<char *>(speech.data()), bytes)) throw std::runtime_error("Could not read the probe input.");
  auto source_path = result;
  source_path += L".source.packets";
  result += L".sink.packets";
  if (std::filesystem::exists(source_path) || std::filesystem::exists(result)) {
    throw std::runtime_error("Use a fresh probe result name.");
  }
  Event render_event;
  Event loop_event;
  Event sink_event;
  auto render = audio_client(source, render_event);
  auto loopback = audio_client(source, loop_event, true);
  auto capture = audio_client(sink, sink_event);
  ProbeRecorder source_dump(loopback.Get(), source_path);
  ProbeRecorder sink_dump(capture.Get(), result);
  ComPtr<IAudioRenderClient> writer;
  check(render->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(writer.GetAddressOf())),
        "Get probe render service");
  UINT32 capacity;
  check(render->GetBufferSize(&capacity), "Get probe render size");
  size_t cursor = 0;
  constexpr size_t lead_in = 48000 / 2;
  const auto feed = [&] {
    UINT32 padding;
    check(render->GetCurrentPadding(&padding), "Get probe render padding");
    const auto count = capacity - padding;
    if (count == 0) return;
    BYTE *data;
    check(writer->GetBuffer(count, &data), "Get probe render buffer");
    auto samples = reinterpret_cast<float *>(data);
    for (UINT32 index = 0; index < count; ++index, ++cursor) {
      samples[index] = cursor >= lead_in && cursor - lead_in < speech.size()
          ? speech[cursor - lead_in] / pcm_scale : 0.0f;
    }
    check(writer->ReleaseBuffer(count, 0), "Release probe render buffer");
  };
  check(capture->Start(), "Start probe sink");
  check(loopback->Start(), "Start probe source loopback");
  feed();
  // Record the playback clock so burst onsets can be placed on the render timeline.
  ComPtr<IAudioClock> clock;
  check(render->GetService(IID_PPV_ARGS(clock.GetAddressOf())), "Get probe render clock");
  UINT64 clock_frequency;
  check(clock->GetFrequency(&clock_frequency), "Get probe clock frequency");
  auto clock_path = result;
  clock_path.replace_extension(L".clock");
  std::ofstream clock_log(clock_path, std::ios::binary | std::ios::trunc);
  if (!clock_log) throw std::runtime_error("Could not open the probe clock file.");
  const UINT64 lead = lead_in;
  clock_log.write(reinterpret_cast<const char *>(&clock_frequency), sizeof(clock_frequency));
  clock_log.write(reinterpret_cast<const char *>(&lead), sizeof(lead));
  check(render->Start(), "Start probe playback");
  const HANDLE events[] = {render_event.handle, loop_event.handle, sink_event.handle};
  const auto deadline = Clock::now() + std::chrono::milliseconds(2000 + 1000 * speech.size() / 48000);
  while (Clock::now() < deadline) {
    if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 10) == WAIT_FAILED) {
      throw std::runtime_error("Waiting for the probe endpoints failed.");
    }
    source_dump.drain();
    sink_dump.drain();
    feed();
    UINT64 position, position_time;
    if (clock->GetPosition(&position, &position_time) == S_OK && position_time) {
      clock_log.write(reinterpret_cast<const char *>(&position), sizeof(position));
      clock_log.write(reinterpret_cast<const char *>(&position_time), sizeof(position_time));
    }
  }
  check(render->Stop(), "Stop probe playback");
  check(loopback->Stop(), "Stop probe loopback");
  check(capture->Stop(), "Stop probe sink");
  std::wcout << L"Wrote probe packets to " << source_path << L" and " << result << L"\n";
}

template<size_t Capacity>
struct AudioQueue {
  std::array<float, Capacity> data{};
  std::array<UINT64, Capacity> timestamps{};
  size_t read = 0;
  size_t size = 0;
  size_t overflow = 0;
  size_t underflow = 0;
  size_t peak = 0;

  void push(float value, UINT64 timestamp = 0) {
    if (size == data.size()) {
      read = (read + 1) % data.size();
      --size;
      ++overflow;
    }
    const auto index = (read + size) % data.size();
    data[index] = value;
    timestamps[index] = timestamp;
    ++size;
  }

  float pop(bool count_underflow = true, UINT64 *timestamp = nullptr) {
    if (size == 0) {
      if (count_underflow) ++underflow;
      if (timestamp) *timestamp = 0;
      return 0.0f;
    }
    const float value = data[read];
    if (timestamp) *timestamp = timestamps[read];
    read = (read + 1) % data.size();
    --size;
    return value;
  }

  // Drops leading near-silent samples until at most keep samples remain.
  size_t drop_quiet(size_t keep, float limit) {
    size_t dropped = 0;
    while (size > keep && std::abs(data[read]) < limit) {
      read = (read + 1) % data.size();
      --size;
      ++dropped;
    }
    return dropped;
  }
};

using Queue = AudioQueue<echo_frame_samples * 3>;

struct TimingSamples {
  static constexpr UINT64 resolution_us = 100;
  std::array<size_t, 2501> bins{};
  size_t count = 0;
  UINT64 total_us = 0;
  UINT64 longest_us = 0;

  void observe(UINT64 microseconds) {
    const auto bin = std::min<UINT64>(microseconds / resolution_us, bins.size() - 1);
    ++bins[static_cast<size_t>(bin)];
    ++count;
    total_us += microseconds;
    longest_us = std::max(longest_us, microseconds);
  }

  double mean_ms() const { return count ? total_us / (1000.0 * count) : 0.0; }

  void print_p95() const {
    size_t seen = 0;
    const auto threshold = (95 * count + 99) / 100;
    for (size_t bin = 0; bin < bins.size(); ++bin) {
      seen += bins[bin];
      if (seen >= threshold) {
        std::cout << (bin * resolution_us / 1000.0) << (bin == bins.size() - 1 ? "+ ms" : " ms");
        break;
      }
    }
  }
};

struct CaptureTrace {
  struct Gap {
    Clock::duration between_packets;
    Clock::duration longest_poll;
  };

  Clock::time_point last_poll;
  Clock::time_point last_packet;
  Clock::duration longest_poll{};
  Clock::duration longest_poll_since_packet{};
  std::array<Gap, 16> gaps{};
  size_t recorded = 0;

  void poll(Clock::time_point now) {
    const auto interval = now - last_poll;
    longest_poll = std::max(longest_poll, interval);
    longest_poll_since_packet = std::max(longest_poll_since_packet, interval);
    last_poll = now;
  }

  void packet(Clock::time_point now, bool gap) {
    if (gap && recorded < gaps.size()) gaps[recorded++] = {now - last_packet, longest_poll_since_packet};
    last_packet = now;
    longest_poll_since_packet = {};
  }

  void report() const {
    const auto ms = [](Clock::duration duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    };
    std::cout << "Longest interval between audio service passes " << ms(longest_poll) << " ms\n";
    for (size_t index = 0; index < recorded; ++index) {
      std::cout << "Capture gap " << index + 1 << " after " << ms(gaps[index].between_packets)
                << " ms between packets, longest service interval " << ms(gaps[index].longest_poll) << " ms\n";
    }
  }
};

struct RenderPacer {
  ComPtr<IAudioClockAdjustment> clock;
  Clock::time_point next_update = Clock::now() + std::chrono::milliseconds(150);
  size_t minimum_buffered = std::numeric_limits<size_t>::max();
  UINT32 rate = static_cast<UINT32>(processing_rate);
  unsigned changes = 0;
  Clock::duration longest_change{};

  void observe(size_t buffered) {
    if (!clock) return;
    minimum_buffered = std::min(minimum_buffered, buffered);
    const auto now = Clock::now();
    if (now < next_update) return;

    // Require a persistent backlog rather than reacting to one model hop.
    const size_t threshold = rate == processing_rate ? samples_per_frame / 2 : samples_per_frame / 4;
    const UINT32 next_rate = static_cast<UINT32>(minimum_buffered >= threshold ? processing_rate * 101 / 100
                                                                              : processing_rate);
    if (next_rate != rate) {
      const auto adjusted = clock->SetSampleRate(static_cast<float>(next_rate));
      longest_change = std::max(longest_change, Clock::now() - now);
      if (FAILED(adjusted) && rate == processing_rate) {
        clock.Reset();
        return;
      }
      check(adjusted, "Restore render sample rate");
      rate = next_rate;
      ++changes;
    }
    minimum_buffered = std::numeric_limits<size_t>::max();
    next_update = now + std::chrono::milliseconds(150);
  }
};

struct AudioPriority {
  DWORD index = 0;
  HANDLE handle = AvSetMmThreadCharacteristicsW(L"Audio", &index);
  ~AudioPriority() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};

class RoomEchoAudio {
 public:
  static constexpr int taps = 63;
  RoomEchoAudio() {
    constexpr double pi = 3.14159265358979323846;
    double gain = 0.0;
    for (int index = 0; index < taps; ++index) {
      const double x = pi * (index - (taps - 1) / 2) / 3.0;
      const double window = 0.54 - 0.46 * std::cos(2.0 * pi * index / (taps - 1));
      filter[index] = static_cast<float>((x == 0.0 ? 1.0 : std::sin(x) / x) * window);
      gain += filter[index];
    }
    for (auto &value : filter) value = static_cast<float>(value / gain);
  }

  void process(float *samples) {
    std::copy(input_history.begin(), input_history.end(), input.data());
    std::copy_n(samples, echo_frame_samples, input.data() + input_history.size());
    std::copy_n(input.end() - input_history.size(), input_history.size(), input_history.data());
    std::array<float, echo_hop> downsampled{};
    for (int index = 0; index < echo_hop; ++index) {
      for (int tap = 0; tap < taps; ++tap) {
        downsampled[index] += filter[tap] * input[index * 3 + taps - 1 - tap];
      }
    }

    std::copy(output_history.begin(), output_history.end(), output.data());
    float *processed = output.data() + output_history.size();
    if (rnn_winml_room_echo_hop(downsampled.data(), processed) != 0) {
      throw std::runtime_error("AMD NPU room-echo inference failed.");
    }
    std::copy_n(output.end() - output_history.size(), output_history.size(), output_history.data());
    for (int index = 0; index < echo_frame_samples; ++index) {
      float value = 0.0f;
      for (int tap = index % 3; tap < taps; tap += 3) {
        value += filter[tap] * output[output_history.size() + index / 3 - tap / 3];
      }
      samples[index] = 3.0f * value;
    }
  }

 private:
  std::array<float, taps> filter{};
  std::array<float, taps - 1> input_history{};
  std::array<float, (taps - 1) / 3> output_history{};
  std::array<float, echo_frame_samples + taps - 1> input{};
  std::array<float, echo_hop + (taps - 1) / 3> output{};
};

// Access the two models on the same audio worker while keeping their recurrent states separate.
class ScopedWinMLSession {
 public:
  explicit ScopedWinMLSession(void *session) : previous_(rnn_winml_exchange_session(session)) {}
  ~ScopedWinMLSession() { rnn_winml_exchange_session(previous_); }
 private:
  void *previous_;
};

class EffectMix {
 public:
  EffectMix(size_t delay, const std::atomic<int> *percent)
      : delay_(delay), percent_(percent), wet_(strength()) {}

  template<typename Process>
  void apply(float *samples, UINT64 *timestamps, size_t count, Process process) {
    std::array<float, echo_frame_samples> dry;
    for (size_t i = 0; i < count; ++i) {
      dry[i] = std::exchange(history_[cursor_], samples[i]);
      timestamps[i] = std::exchange(times_[cursor_], timestamps[i]);
      if (++cursor_ == delay_) cursor_ = 0;
    }
    process(samples);
    const float target = strength();
    if (wet_ != 1.0f || target != 1.0f) {
      for (size_t i = 0; i < count; ++i) {
        const float mix = wet_ + (target - wet_) * static_cast<float>(i + 1) / count;
        samples[i] = dry[i] + mix * (samples[i] - dry[i]);
      }
    }
    wet_ = target;
  }

 private:
  float strength() const { return percent_ ? percent_->load() / 100.0f : 1.0f; }

  std::array<float, 2 * samples_per_frame> history_{};
  std::array<UINT64, 2 * samples_per_frame> times_{};
  size_t cursor_ = 0;
  size_t delay_;
  const std::atomic<int> *percent_;
  float wet_;
};

struct AudioPipeline {
  std::mutex lock;
  std::condition_variable ready;
  Queue captured;
  AudioQueue<echo_frame_samples * 6> rendered;
  Event output_ready;
  bool stopped = false;
  std::exception_ptr failure;
  TimingSamples frames;
  TimingSamples inferences;
  size_t overruns = 0;
  std::thread worker;

  AudioPipeline(DenoiseState *state, bool room_echo, void *echo_session, AudioStatus *status)
      : worker([this, state, room_echo, echo_session, status] { process(state, room_echo, echo_session, status); }) {}
  ~AudioPipeline() { stop(); }

  void stop() {
    {
      std::lock_guard<std::mutex> guard(lock);
      stopped = true;
    }
    ready.notify_one();
    if (worker.joinable()) worker.join();
  }

  void process(DenoiseState *state, bool room_echo, void *echo_session, AudioStatus *status) noexcept {
    try {
      ComApartment apartment(COINIT_MULTITHREADED);
      AudioPriority priority;
      std::array<float, 2 * samples_per_frame> frame{};
      std::array<UINT64, 2 * samples_per_frame> frame_timestamps{};
      const size_t frame_size = room_echo ? echo_frame_samples : samples_per_frame;
      const UINT64 frame_budget_us = frame_size * 1000000 / processing_rate;
      std::optional<RoomEchoAudio> echo;
      if (room_echo) echo.emplace();
      EffectMix echo_mix(echo_frame_samples + RoomEchoAudio::taps - 1, status ? &status->echo_strength_percent : nullptr);
      EffectMix noise_mix(2 * samples_per_frame, status ? &status->noise_strength_percent : nullptr);
      Queue echo_output;
      std::array<float, samples_per_frame> noise_frame{};
      std::array<UINT64, samples_per_frame> noise_timestamps{};
      auto denoise = [state](float *samples) {
        for (int i = 0; i < samples_per_frame; ++i) samples[i] *= pcm_scale;
        rnnoise_process_frame(state, samples, samples);
        for (int i = 0; i < samples_per_frame; ++i) samples[i] /= pcm_scale;
      };
      for (;;) {
        {
          std::unique_lock<std::mutex> guard(lock);
          ready.wait(guard, [&] { return stopped || captured.size >= frame_size; });
          if (stopped) break;
          for (size_t index = 0; index < frame_size; ++index) {
            frame[index] = captured.pop(true, &frame_timestamps[index]);
          }
        }
        const auto start = Clock::now();
        size_t output_count = frame_size;
        long long inference = -1;
        auto sample_inference = [&] {
          const auto elapsed = rnn_winml_take_inference_us();
          if (elapsed >= 0) inference = std::max(0LL, inference) + elapsed;
        };
        if (echo) {
          echo_mix.apply(frame.data(), frame_timestamps.data(), frame_size, [&](float *samples) {
            if (echo_session) {
              ScopedWinMLSession session(echo_session);
              echo->process(samples);
            } else {
              echo->process(samples);
            }
          });
          sample_inference();
        }
        if (echo && state) {
          for (size_t i = 0; i < frame_size; ++i) echo_output.push(frame[i], frame_timestamps[i]);
          output_count = 0;
          // 16 ms echo hops feed 10 ms RNNoise frames. Keep the remainder for the next hop.
          while (echo_output.size >= samples_per_frame && output_count + samples_per_frame <= frame.size()) {
            for (size_t i = 0; i < noise_frame.size(); ++i) {
              noise_frame[i] = echo_output.pop(true, &noise_timestamps[i]);
            }
            noise_mix.apply(noise_frame.data(), noise_timestamps.data(), noise_frame.size(), denoise);
            sample_inference();
            std::copy(noise_frame.begin(), noise_frame.end(), frame.begin() + output_count);
            std::copy(noise_timestamps.begin(), noise_timestamps.end(), frame_timestamps.begin() + output_count);
            output_count += noise_frame.size();
          }
        } else if (state) {
          noise_mix.apply(frame.data(), frame_timestamps.data(), frame_size, denoise);
          sample_inference();
        }
        const auto frame_us = static_cast<UINT64>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        frames.observe(frame_us);
        if (frame_us > frame_budget_us) ++overruns;
        if (inference >= 0) {
          inferences.observe(static_cast<UINT64>(inference));
          if (status) status->inference_us = inference;
        }
        {
          std::lock_guard<std::mutex> guard(lock);
          for (size_t i = 0; i < output_count; ++i) {
            const float value = frame[i];
            rendered.push(std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f, frame_timestamps[i]);
          }
          rendered.peak = std::max(rendered.peak, rendered.size);
        }
        SetEvent(output_ready.handle);
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> guard(lock);
        failure = std::current_exception();
      }
      SetEvent(output_ready.handle);
    }
  }
};

void configure_npu(bool room_echo) {
  wchar_t path[32768];
  const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (!length || length == ARRAYSIZE(path)) throw std::runtime_error("Could not locate the executable.");
  auto models = std::filesystem::path(path).parent_path() / L"models";
  if (room_echo) models /= L"room-echo";
  const auto graph = models / (room_echo ? L"room_echo_host.onnx" : L"rnnoise_host.onnx");
  if (!std::filesystem::is_regular_file(graph)) throw std::runtime_error("Could not find the requested NPU model.");
  if (_wputenv_s(L"RNNOISE_WINML_MODEL", graph.c_str()) ||
      _wputenv_s(L"RNNOISE_WINML_MATRIX_DIR", models.c_str()) ||
      _wputenv_s(L"RNNOISE_WINML_ROOM_ECHO", room_echo ? L"1" : L"") ||
      _wputenv_s(L"RNNOISE_WINML_ECHO_OVERLAP_ADD", room_echo ? L"1" : L"") ||
      _wputenv_s(L"RNNOISE_WINML_EP", L"npu") ||
      _wputenv_s(L"RNNOISE_WINML_EP_DEVICE_INDEX", L"") ||
      _wputenv_s(L"RNNOISE_WINML_DEVICE_INDEX", L"") ||
      _wputenv_s(L"RNNOISE_WINML_ALLOW_CPU_PARTITIONS", L"") ||
      _wputenv_s(L"RNNOISE_WINML_CALIBRATION", L"")) {
    throw std::runtime_error("Could not configure AMD NPU inference.");
  }
  if (!_wgetenv(L"RNNOISE_WINML_REPORT_CACHE")) {
    if (const auto base = _wgetenv(L"LOCALAPPDATA")) {
      const auto cache = std::filesystem::path(base) / L"NPU Broadcast" / L"VitisAI";
      std::error_code error;
      std::filesystem::create_directories(cache, error);
      if (!error) _wputenv_s(L"RNNOISE_WINML_REPORT_CACHE", cache.c_str());
    }
  }
  std::cout << "Preparing the AMD NPU " << (room_echo ? "room-echo and noise" : "noise-removal")
            << " model.\n" << std::flush;
  if (rnn_winml_prepare() != 0) throw std::runtime_error("Could not load the requested AMD NPU model.");
}

class RouteEffects {
  struct SessionLifetime {
    void *echo = nullptr;
    ~SessionLifetime() {
      if (echo) {
        ScopedWinMLSession session(echo);
        rnn_winml_shutdown();
      }
      rnn_winml_shutdown();
    }
  };

 public:
  RouteEffects(Effect effect, AudioStatus *status, bool load_only = false) {
    const bool room_echo = effect == Effect::room_echo || effect == Effect::combined;
    const bool noise = effect == Effect::noise || effect == Effect::combined;
    if (noise) {
      state_.reset(rnnoise_create(nullptr));
      if (!state_) throw std::runtime_error("Could not create the denoiser.");
    }
    if (effect != Effect::bypass || load_only) {
      if (effect == Effect::combined) {
        configure_npu(false);
        ScopedWinMLSession park_noise(nullptr);
        configure_npu(true);
        session_.echo = rnn_winml_exchange_session(nullptr);
      } else {
        configure_npu(room_echo);
      }
      if (status) status->npu_device_id = rnn_winml_npu_device_id();
    }
    pipeline_.emplace(state_.get(), room_echo, session_.echo, status);
  }

  AudioPipeline &pipeline() { return *pipeline_; }

 private:
  SessionLifetime session_;
  std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> state_{nullptr, rnnoise_destroy};
  std::optional<AudioPipeline> pipeline_;
};

// Keep physical capture and the driver handle open across model starts and stops.
// Both paths carry the same sample indices so late inference falls back to dry audio.
class DesktopEffects {
 public:
  explicit DesktopEffects(AudioStatus &status) : status_(status) {}

  AudioPipeline *update(Effect requested) {
    if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        processor_ = pending_.get();
      } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        failed_ = mode_;
      }
    }
    if (requested != mode_ && !pending_.valid()) {
      failed_.reset();
      transition(requested);
    }
    if (processor_ && requested == mode_) {
      bool failed;
      {
        auto &pipeline = processor_->pipeline();
        std::lock_guard<std::mutex> guard(pipeline.lock);
        failed = static_cast<bool>(pipeline.failure);
      }
      if (failed) {
        failed_ = mode_;
        transition(Effect::bypass);
        mode_ = requested;
      }
    }
    status_.effects_unavailable = failed_ == requested;
    return processor_ && requested == mode_ ? &processor_->pipeline() : nullptr;
  }

 private:
  void transition(Effect next) {
    mode_ = next;
    status_.inference_us = -1;
    status_.npu_device_id = 0;
    pending_ = std::async(std::launch::async, [old = std::move(processor_), next, status = &status_]() mutable {
      ComApartment apartment(COINIT_MULTITHREADED);
      old.reset();
      return next == Effect::bypass ? nullptr : std::make_unique<RouteEffects>(next, status);
    });
  }

  AudioStatus &status_;
  Effect mode_ = Effect::bypass;
  std::optional<Effect> failed_;
  std::unique_ptr<RouteEffects> processor_;
  std::future<std::unique_ptr<RouteEffects>> pending_;
};

// Monitoring is a best-effort copy. A full or disconnected playback buffer
// must never delay delivery to the virtual microphone.
class VoiceMonitor {
 public:
  void update(AudioStatus &status) {
    if (!status.listen) {
      close(status);
      return;
    }
    const auto now = Clock::now();
    if (now < refresh_) return;
    refresh_ = now + std::chrono::milliseconds(500);
    try {
      const auto id = default_endpoint_id(true);
      if (client_ && id == monitor_id_) return;
      close(status);
      if (id.empty()) return;
      const auto outputs = active_endpoints(true);
      const auto target = std::find_if(outputs.begin(), outputs.end(),
                                       [&](const Endpoint &endpoint) { return endpoint.id == id; });
      if (target == outputs.end() || target->cable ||
          target->name.find(L"(VB-Audio Virtual Cable)") != std::wstring::npos) return;
      auto enumerator = device_enumerator();
      ComPtr<IMMDevice> device;
      check(enumerator->GetDevice(id.c_str(), &device), "Open monitor endpoint");
      client_ = audio_client(device.Get(), event_, false);
      check(client_->GetService(IID_PPV_ARGS(writer_.GetAddressOf())), "Open monitor render service");
      check(client_->GetBufferSize(&capacity_), "Get monitor capacity");
      check(client_->Start(), "Start voice monitor");
      monitor_id_ = id;
      status.monitor_available = true;
    } catch (const std::exception &) {
      close(status);
    }
  }

  void copy(const float *samples, UINT32 count, AudioStatus &status) {
    if (!client_ || !status.listen || !count) return;
    UINT32 padding;
    if (FAILED(client_->GetCurrentPadding(&padding))) {
      close(status);
      return;
    }
    count = std::min(count, capacity_ - std::min(capacity_, padding));
    if (!count) return;
    BYTE *buffer;
    if (FAILED(writer_->GetBuffer(count, &buffer))) {
      close(status);
      return;
    }
    std::copy_n(samples, count, reinterpret_cast<float *>(buffer));
    if (FAILED(writer_->ReleaseBuffer(count, 0))) close(status);
  }

 private:
  void close(AudioStatus &status) {
    if (client_) client_->Stop();
    writer_.Reset();
    client_.Reset();
    status.monitor_available = false;
  }

  Event event_;
  std::wstring monitor_id_;
  Clock::time_point refresh_{};
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> writer_;
  UINT32 capacity_ = 0;
};

class DesktopMonitor {
 public:
  explicit DesktopMonitor(AudioStatus &status) : status_(status), worker_([this] { play(); }) {}

  ~DesktopMonitor() {
    {
      std::lock_guard<std::mutex> guard(lock_);
      stopped_ = true;
    }
    wake_.notify_one();
    worker_.join();
  }

  void copy(const float *samples, UINT32 count) {
    if (!status_.listen) return;
    {
      std::lock_guard<std::mutex> guard(lock_);
      for (UINT32 i = 0; i < count; ++i) queue_.push(samples[i]);
    }
    wake_.notify_one();
  }

 private:
  void play() noexcept {
    try {
      ComApartment apartment(COINIT_MULTITHREADED);
      VoiceMonitor monitor;
      std::array<float, NPU_MIC_PACKET_SAMPLES> samples;
      for (;;) {
        UINT32 count;
        {
          std::unique_lock<std::mutex> guard(lock_);
          wake_.wait_for(guard, std::chrono::milliseconds(10), [this] {
            return stopped_ || queue_.size > 0;
          });
          if (stopped_) break;
          if (!status_.listen) queue_ = {};
          count = static_cast<UINT32>(std::min(queue_.size, samples.size()));
          for (UINT32 i = 0; i < count; ++i) samples[i] = queue_.pop(false);
        }
        monitor.update(status_);
        monitor.copy(samples.data(), count, status_);
      }
    } catch (const std::exception &error) {
      std::cerr << error.what() << "\n";
    }
    status_.monitor_available = false;
  }

  AudioStatus &status_;
  Queue queue_;
  std::mutex lock_;
  std::condition_variable wake_;
  bool stopped_ = false;
  std::thread worker_;
};

void run(IMMDevice *input, IMMDevice *output, unsigned long seconds, bool check_only, bool muted,
         Effect effect, bool loopback, HANDLE stop_signal_override = nullptr, AudioStatus *status = nullptr,
         HANDLE producer = nullptr) {
  const bool desktop = status != nullptr;
  std::optional<ConsoleStop> console;
  if (!stop_signal_override) console.emplace();
  Event capture_event;
  Event render_event;
  auto capture = audio_client(input, capture_event, loopback);
  ComPtr<IAudioClient> render;
  ComPtr<IAudioClockAdjustment> adjustment;
  ComPtr<IAudioCaptureClient> reader;
  ComPtr<IAudioRenderClient> writer;
  check(capture->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(reader.GetAddressOf())), "Get capture service");
  UINT32 render_capacity = NPU_MIC_PACKET_SAMPLES;
  if (!producer) {
    render = audio_client(output, render_event, false, AUDCLNT_STREAMFLAGS_RATEADJUST);
    if (FAILED(render->GetService(IID_PPV_ARGS(adjustment.GetAddressOf())))) adjustment.Reset();
    check(render->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(writer.GetAddressOf())), "Get render service");
    check(render->GetBufferSize(&render_capacity), "Get render buffer size");
  }
  if (check_only) {
    std::cout << "Opened both WASAPI endpoints at 48 kHz mono. Render capacity "
              << render_capacity << " samples.\n";
    report_period(capture.Get(), "Capture");
    report_period(render.Get(), "Render");
    std::cout << "Render clock adjustment " << (adjustment ? "available" : "unavailable") << ".\n";
    return;
  }

  const bool room_echo = effect == Effect::room_echo || effect == Effect::combined;
  const bool bypass = effect == Effect::bypass;
  const HANDLE stop_event = stop_signal_override ? stop_signal_override : console->event.handle;
  if (render && !loopback) {
    // Wait for playback to begin draining the initial buffer before turning on the microphone.
    BYTE *initial_buffer;
    check(writer->GetBuffer(render_capacity, &initial_buffer), "Get initial render buffer");
    check(writer->ReleaseBuffer(render_capacity, AUDCLNT_BUFFERFLAGS_SILENT), "Prime render with silence");
    check(render->Start(), "Start render");
    const HANDLE startup_events[] = {stop_event, render_event.handle};
    const DWORD event = WaitForMultipleObjects(ARRAYSIZE(startup_events), startup_events, FALSE, 1000);
    if (event == WAIT_FAILED) throw std::runtime_error("Waiting for render startup failed.");
    if (event == WAIT_OBJECT_0) {
      check(render->Stop(), "Stop render");
      return;
    }
  }
  std::optional<RouteEffects> fixed_effects;
  std::optional<DesktopEffects> desktop_effects;
  if (desktop) desktop_effects.emplace(*status);
  else fixed_effects.emplace(effect, status, muted && _wgetenv(L"NPU_BROADCAST_TEST_LOAD_ONLY"));
  if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
    if (render && !loopback) check(render->Stop(), "Stop render");
    return;
  }
  AudioPipeline *pipeline = fixed_effects ? &fixed_effects->pipeline() : nullptr;
  AudioQueue<NPU_MIC_PACKET_SAMPLES * 12> dry;
  UINT64 sample_index = 0;
  RenderPacer pacer{std::move(adjustment)};
  size_t capture_gaps = 0;
  size_t trimmed_samples = 0;
  std::chrono::milliseconds first_gap{};
  std::chrono::milliseconds last_gap{};
  std::array<std::chrono::milliseconds, 8> first_packets{};
  size_t sampled_packets = 0;
  bool captured_packet = false;
  Clock::time_point last_capture;
  bool rendering = false;
  const UINT32 target_samples = room_echo ? 960 : 720;
  const UINT32 render_target = std::min(render_capacity, target_samples);
  TimingSamples delay_samples;
  LARGE_INTEGER qpc_frequency{};
  const bool measure_delay = QueryPerformanceFrequency(&qpc_frequency) != FALSE;

  std::optional<DesktopMonitor> monitor;
  if (desktop) monitor.emplace(*status);
  AudioPriority priority;
  check(capture->Start(), "Start capture");
  const auto capture_start = Clock::now();
  CaptureTrace capture_trace{capture_start, capture_start};
  const auto deadline = seconds ? capture_start + std::chrono::seconds(seconds) : Clock::time_point::max();
  const HANDLE events[] = {stop_event, capture_event.handle, render_event.handle,
                           pipeline ? pipeline->output_ready.handle : nullptr};
  const DWORD event_count = desktop ? (producer ? 2 : 3) : ARRAYSIZE(events);
  std::cout << (bypass ? "Effects off. " : "AMD NPU active. ")
            << (muted ? "Silent output. " : "") << "Press Ctrl+C to stop.\n" << std::flush;
  if (loopback && status) status->active = true;
  while (Clock::now() < deadline) {
    // Also drain the endpoints on a missed/coalesced device event.
    const DWORD event = WaitForMultipleObjects(event_count, events, FALSE, 10);
    if (event == WAIT_OBJECT_0) break;
    if (event == WAIT_FAILED) throw std::runtime_error("Waiting for audio failed.");
    const Effect requested = desktop ? status->requested_effect.load() : effect;
    if (desktop_effects) pipeline = desktop_effects->update(requested);
    if (muted) capture_trace.poll(Clock::now());
    float input_peak = 0.0f;
    UINT32 available;
    check(reader->GetNextPacketSize(&available), "GetNextPacketSize");
    while (available) {
      BYTE *data;
      UINT32 samples;
      DWORD flags;
      UINT64 captured_at = 0;
      check(reader->GetBuffer(&data, &samples, &flags, nullptr, measure_delay ? &captured_at : nullptr),
            "Get capture buffer");
      if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) captured_at = 0;
      last_capture = Clock::now();
      if (muted && sampled_packets < first_packets.size()) {
        first_packets[sampled_packets++] =
            std::chrono::duration_cast<std::chrono::milliseconds>(last_capture - capture_start);
      }
      const bool gap = captured_packet && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
      if (muted) capture_trace.packet(last_capture, gap);
      if (gap) {
        last_gap = std::chrono::duration_cast<std::chrono::milliseconds>(last_capture - capture_start);
        if (!capture_gaps) first_gap = last_gap;
        ++capture_gaps;
        if (status) status->capture_gaps = capture_gaps;
      }
      captured_packet = true;
      const auto source = reinterpret_cast<const float *>(data);
      std::unique_lock<std::mutex> guard;
      if (pipeline) guard = std::unique_lock<std::mutex>(pipeline->lock);
      for (UINT32 index = 0; index < samples; ++index) {
        const auto sample = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0.0f : source[index];
        const float bounded = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
        const auto timestamp = desktop ? ++sample_index :
            captured_at ? captured_at + index * ticks_per_second / processing_rate : 0;
        if (desktop) dry.push(bounded, timestamp);
        if (pipeline) pipeline->captured.push(bounded, timestamp);
        input_peak = std::max(input_peak, std::abs(bounded));
      }
      if (pipeline) {
        pipeline->captured.peak = std::max(pipeline->captured.peak, pipeline->captured.size);
        guard.unlock();
        pipeline->ready.notify_one();
      }
      check(reader->ReleaseBuffer(samples), "Release capture buffer");
      check(reader->GetNextPacketSize(&available), "GetNextPacketSize");
    }
    if (status) status->input_level = std::max(input_peak, status->input_level.load() * 0.92f);
    if (desktop) {
      // Combined output includes both model delays, 16-to-10 ms frame batching and inference.
      const size_t desktop_delay = NPU_MIC_RATE * (requested == Effect::combined ? 90 : 40) / 1000;
      std::array<float, NPU_MIC_PACKET_SAMPLES> samples;
      std::array<short, NPU_MIC_PACKET_SAMPLES> pcm;
      UINT32 free = UINT32_MAX;
      if (render) {
        UINT32 padding = 0;
        check(render->GetCurrentPadding(&padding), "Get cable render padding");
        free = render_target > padding ? render_target - padding : 0;
      }
      while (dry.size > desktop_delay && free) {
        const auto count = static_cast<UINT32>(std::min({dry.size - desktop_delay, samples.size(), static_cast<size_t>(free)}));
        float peak = 0;
        bool processed = false;
        std::unique_lock<std::mutex> guard;
        if (pipeline) guard = std::unique_lock<std::mutex>(pipeline->lock);
        for (UINT32 i = 0; i < count; ++i) {
          UINT64 stamp;
          samples[i] = dry.pop(true, &stamp);
          if (pipeline && !pipeline->failure) {
            auto &wet = pipeline->rendered;
            while (wet.size && wet.timestamps[wet.read] < stamp) wet.pop(false);
            if (wet.size && wet.timestamps[wet.read] == stamp) {
              samples[i] = wet.pop(false);
              processed = true;
            }
          }
          if (producer) pcm[i] = static_cast<short>(std::lround(samples[i] * 32767.0f));
          peak = std::max(peak, std::abs(samples[i]));
        }
        if (pipeline) {
          status->discarded = dry.overflow + pipeline->captured.overflow + pipeline->rendered.overflow;
          guard.unlock();
        }
        if (producer) {
          DWORD bytes;
          if (!DeviceIoControl(producer, IOCTL_NPU_MIC_WRITE, pcm.data(), count * sizeof(short),
                               nullptr, 0, &bytes, nullptr)) {
            check(HRESULT_FROM_WIN32(GetLastError()), "Write virtual microphone");
          }
        } else {
          BYTE *data;
          check(writer->GetBuffer(count, &data), "Get cable buffer");
          std::copy_n(samples.data(), count, reinterpret_cast<float *>(data));
          check(writer->ReleaseBuffer(count, 0), "Write VB-CABLE");
          free -= count;
        }
        status->effects_delivered = processed;
        status->output_level = std::max(peak, status->output_level.load() * 0.92f);
        status->active = true;
        monitor->copy(samples.data(), count);
      }
      if (render) pacer.observe(dry.size > desktop_delay ? dry.size - desktop_delay : 0);
      continue;
    }
    size_t queued;
    {
      std::lock_guard<std::mutex> guard(pipeline->lock);
      if (pipeline->failure) std::rethrow_exception(pipeline->failure);
      const size_t hop = room_echo ? echo_frame_samples : samples_per_frame;
      if (rendering && pipeline->rendered.size > 2 * hop) {
        trimmed_samples += pipeline->rendered.drop_quiet(hop, 0.002f);
      }
      queued = pipeline->rendered.size;
    }
    if (rendering || queued >= render_target) {
      // A playback endpoint stops supplying loopback packets when its clients go idle.
      const bool source_active = !loopback ||
          (captured_packet && Clock::now() - last_capture <= std::chrono::milliseconds(30));
      UINT32 padding;
      check(render->GetCurrentPadding(&padding), "Get render padding");
      const UINT32 free_target = padding >= render_target ? 0 : render_target - padding;
      // Existing playback padding gives the worker time to supply real audio.
      UINT32 count = static_cast<UINT32>(std::min<size_t>(free_target, queued));
      if (rendering && padding == 0 && count < samples_per_frame) {
        count = std::min<UINT32>(render_target, samples_per_frame);
      }
      if (count) {
        BYTE *data;
        float output_peak = 0.0f;
        check(writer->GetBuffer(count, &data), "Get render buffer");
        auto destination = reinterpret_cast<float *>(data);
        UINT64 source_time = 0;
        UINT32 source_index = 0;
        {
          std::lock_guard<std::mutex> guard(pipeline->lock);
          for (UINT32 index = 0; index < count; ++index) {
            UINT64 captured_at = 0;
            destination[index] = pipeline->rendered.pop(source_active, measure_delay ? &captured_at : nullptr);
            if (!source_time && captured_at) {
              source_time = captured_at;
              source_index = index;
            }
            output_peak = std::max(output_peak, std::abs(destination[index]));
          }
          queued = pipeline->rendered.size;
          if (status) {
            status->missing = pipeline->rendered.underflow;
            status->discarded = pipeline->captured.overflow + pipeline->rendered.overflow;
          }
        }
        if (status) status->output_level = std::max(output_peak, status->output_level.load() * 0.92f);
        check(writer->ReleaseBuffer(count, muted ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "Release render buffer");
        if (source_time && rendering) {
          LARGE_INTEGER now;
          if (QueryPerformanceCounter(&now)) {
            const auto ticks = static_cast<UINT64>(static_cast<double>(now.QuadPart) *
                                                  ticks_per_second / qpc_frequency.QuadPart);
            const auto delivery = ticks + (padding + source_index) * ticks_per_second / pacer.rate;
            if (delivery >= source_time) delay_samples.observe((delivery - source_time) / 10);
          }
        }
      }
      if (!rendering && count) {
        if (loopback) check(render->Start(), "Start render");
        rendering = true;
        if (status) status->active = true;
      }
      if (rendering) pacer.observe(source_active ? queued : 0);
    }
  }
  check(capture->Stop(), "Stop capture");
  if (render && (rendering || !loopback)) check(render->Stop(), "Stop render");
  if (desktop) return;
  pipeline->stop();
  if (pipeline->failure) std::rethrow_exception(pipeline->failure);
  std::cout << "Processed frames " << pipeline->frames.count << ", longest frame " << pipeline->frames.longest_us / 1000.0
            << " ms, capture gaps " << capture_gaps << ", missing samples " << pipeline->rendered.underflow
            << ", discarded samples " << pipeline->captured.overflow + pipeline->rendered.overflow << "\n";
  if (pipeline->captured.overflow || pipeline->rendered.overflow) {
    std::cout << "Queue discards: " << pipeline->captured.overflow << " capture, "
              << pipeline->rendered.overflow << " render samples\n";
  }
  if (capture_gaps) {
    std::cout << "Capture gap detection after " << first_gap.count() << " ms (first) and "
              << last_gap.count() << " ms (last) from capture start\n";
  }
  if (sampled_packets) {
    std::cout << "First capture packet arrivals in ms:";
    for (size_t index = 0; index < sampled_packets; ++index) std::cout << ' ' << first_packets[index].count();
    std::cout << '\n';
  }
  if (muted) capture_trace.report();
  if (pipeline->frames.count) {
    std::cout << "Frame processing mean " << pipeline->frames.mean_ms() << " ms, P95 ";
    pipeline->frames.print_p95();
    std::cout << ", over budget " << pipeline->overruns << "\n";
    std::cout << "Queue peaks " << pipeline->captured.peak << " capture samples and "
              << pipeline->rendered.peak << " render samples\n";
  }
  if (pipeline->inferences.count) {
    std::cout << pipeline->inferences.count << " model inferences, mean " << pipeline->inferences.mean_ms() << " ms, P95 ";
    pipeline->inferences.print_p95();
    std::cout << '\n';
  }
  if (delay_samples.count) {
    std::cout << "Estimated capture-to-render P95 ";
    delay_samples.print_p95();
    std::cout << " from " << delay_samples.count << " render writes.\n";
  }
  if (trimmed_samples) std::cout << "Trimmed " << trimmed_samples << " quiet samples from the render backlog\n";
  if (pacer.changes) {
    const auto longest_ms = std::chrono::duration<double, std::milli>(pacer.longest_change).count();
    std::cout << "Render rate changes " << pacer.changes << ", last rate " << pacer.rate
              << " Hz, longest rate call " << longest_ms << " ms\n";
  }
}

}  // namespace

std::vector<Endpoint> active_endpoints(bool render) {
  auto enumerator = device_enumerator();
  return describe_endpoints(endpoints(enumerator.Get(), render ? eRender : eCapture).Get());
}

std::wstring default_endpoint_id(bool render) {
  auto enumerator = device_enumerator();
  ComPtr<IMMDevice> device;
  const auto result = enumerator->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &device);
  if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return {};
  check(result, "Get default audio endpoint");
  LPWSTR id = nullptr;
  check(device->GetId(&id), "Get default audio endpoint ID");
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(id, CoTaskMemFree);
  return id;
}

namespace {
std::wstring_view cable_family(const Endpoint &endpoint) {
  const std::wstring_view label = endpoint.name;
  const auto suffix = label.rfind(L" (VB-Audio ");
  if (suffix != std::wstring_view::npos && label.back() == L')') {
    return label.substr(suffix + 2, label.size() - suffix - 3);
  }
  return endpoint.interface_name;
}
}

bool extra_vb_cable(const Endpoint &endpoint) {
  const auto family = cable_family(endpoint);
  return endpoint.cable && !family.empty() &&
      (family.rfind(L"VB-Audio Cable ", 0) == 0 || family.rfind(L"VB-Audio Virtual Cable ", 0) == 0) &&
      family.back() >= L'A' && family.back() <= L'D';
}

Endpoint paired_vb_cable_output(const Endpoint &microphone, const std::vector<Endpoint> &playback) {
  if (!microphone.cable) return {};
  const auto family = cable_family(microphone);
  const auto prefix = extra_vb_cable(microphone) ? L"CABLE-" + std::wstring(1, family.back()) + L" Input" :
      std::wstring(L"CABLE Input");
  Endpoint match;
  for (const auto &output : playback) {
    if (!output.cable || cable_family(output) != family) continue;
    if (output.name != prefix && output.name.rfind(prefix + L" (", 0) != 0) continue;
    if (!match.id.empty()) return {};
    match = output;
  }
  return match;
}

#ifdef NPU_BROADCAST_GUI
// AudioSes policy interface. The ordinary endpoint property store is used for readback.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) AudioEndpointPolicy : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, INT64 *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY *, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY *, PROPVARIANT *) = 0;
};

static int name_vb_cable(const wchar_t *id) {
  try {
    ComApartment apartment;
    const auto captures = active_endpoints(false);
    const auto device = std::find_if(captures.begin(), captures.end(), [&](const Endpoint &endpoint) {
      return endpoint.cable && CompareStringOrdinal(endpoint.id.c_str(), -1, id, -1, TRUE) == CSTR_EQUAL;
    });
    if (device == captures.end() || paired_vb_cable_output(*device, active_endpoints(true)).id.empty()) {
      throw std::runtime_error("The selected VB-CABLE is unavailable.");
    }
    const auto collision = std::find_if(captures.begin(), captures.end(), [](const Endpoint &endpoint) {
      return endpoint.name == NPU_MIC_ENDPOINT ||
          endpoint.name.rfind(std::wstring(NPU_MIC_ENDPOINT) + L" (", 0) == 0;
    });
    if (collision != captures.end()) return 0;
    auto enumerator = device_enumerator();
    ComPtr<IMMDevice> microphone;
    check(enumerator->GetDevice(device->id.c_str(), &microphone), "Open selected cable");
    ComPtr<IPropertyStore> properties;
    const CLSID policy_class = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
    ComPtr<AudioEndpointPolicy> policy;
    check(CoCreateInstance(policy_class, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(policy.GetAddressOf())), "Open Windows audio policy");
    wchar_t label[] = NPU_MIC_ENDPOINT;
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    value.pwszVal = label;
    check(policy->SetPropertyValue(device->id.c_str(), FALSE, &PKEY_Device_DeviceDesc, &value), "Rename cable");
    check(microphone->OpenPropertyStore(STGM_READ, &properties), "Read updated cable name");
    PROPVARIANT actual{};
    const bool renamed = SUCCEEDED(properties->GetValue(PKEY_Device_DeviceDesc, &actual)) &&
        actual.vt == VT_LPWSTR && actual.pwszVal && std::wstring_view(actual.pwszVal) == label;
    PropVariantClear(&actual);
    if (!renamed) throw std::runtime_error("Windows did not keep the new microphone name.");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    MessageBoxW(nullptr, L"Windows could not rename the selected cable. Rename its recording endpoint "
                        L"to Microphone (NPU Broadcast) in Settings > System > Sound > Input.",
                L"NPU Broadcast", MB_OK | MB_ICONWARNING);
    return 1;
  }
}
#endif

bool recording_endpoint_in_use(const Endpoint &endpoint) {
  auto enumerator = device_enumerator();
  ComPtr<IMMDevice> device;
  check(enumerator->GetDevice(endpoint.id.c_str(), &device), "Open virtual microphone");
  ComPtr<IAudioSessionManager2> manager;
  check(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void **>(manager.GetAddressOf())), "Open recording session manager");
  ComPtr<IAudioSessionEnumerator> sessions;
  check(manager->GetSessionEnumerator(&sessions), "List recording sessions");
  int count;
  check(sessions->GetCount(&count), "Count recording sessions");
  for (int index = 0; index < count; ++index) {
    ComPtr<IAudioSessionControl> session;
    AudioSessionState state;
    if (SUCCEEDED(sessions->GetSession(index, &session)) &&
        SUCCEEDED(session->GetState(&state)) && state == AudioSessionStateActive) return true;
  }
  return false;
}

void run_window_route(const Endpoint &input, const Endpoint &cable_output, HANDLE producer,
                      HANDLE stop, AudioStatus &status) {
  ComApartment apartment;
  auto enumerator = device_enumerator();
  ComPtr<IMMDevice> capture;
  ComPtr<IMMDevice> render;
  check(enumerator->GetDevice(input.id.c_str(), &capture), "Open input device");
  if (!producer) {
    check(enumerator->GetDevice(cable_output.id.c_str(), &render), "Open VB-CABLE playback device");
  }
  run(capture.Get(), render.Get(), 0, false, false, Effect::bypass, false, stop, &status, producer);
}

#ifdef NPU_BROADCAST_GUI
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = __argc;
  wchar_t **argv = __wargv;
#else
int wmain(int argc, wchar_t **argv) {
#endif
  bool room_echo = false;
  bool bypass = false;
  bool loopback = false;
  while (argc > 2) {
    const std::wstring flag = argv[argc - 1];
    if (flag == L"--room-echo") room_echo = true;
    else if (flag == L"--bypass") bypass = true;
    else if (flag == L"--loopback") loopback = true;
    else break;
    --argc;
  }
  if (room_echo && bypass) {
    std::cerr << "Choose either --room-echo or --bypass.\n";
    return 1;
  }
  const Effect effect = room_echo ? Effect::room_echo : bypass ? Effect::bypass : Effect::noise;
#ifdef NPU_BROADCAST_GUI
  if (argc == 3 && std::wstring_view(argv[1]) == L"--name-vb-cable") return name_vb_cable(argv[2]);
  if (argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--gui-smoke")) {
    try {
      ComApartment apartment;
      return show_window(argc == 2);
    } catch (const std::exception &error) {
      std::cerr << "NPU Broadcast: " << error.what() << "\n";
      return 1;
    }
  }
#endif
  if (argc == 6 && std::wstring(argv[1]) == L"route-probe") {
    try {
      ComApartment apartment;
      auto enumerator = device_enumerator();
      auto outputs = endpoints(enumerator.Get(), eRender);
      auto inputs = endpoints(enumerator.Get(), eCapture);
      auto playback = device_at(outputs.Get(), number(argv[2]));
      auto capture = device_at(inputs.Get(), number(argv[3]));
      route_probe(playback.Get(), capture.Get(), argv[4], argv[5]);
      return 0;
    } catch (const std::exception &error) {
      std::cerr << "Audio route probe: " << error.what() << "\n";
      return 1;
    }
  }
  if ((argc != 2 && argc != 4 && argc != 5) ||
      (argc == 2 && std::wstring(argv[1]) == L"--gui-smoke")) {
    std::wcerr << L"Usage: npu-broadcast list | npu-broadcast check <input-index> <output-index>\n"
               << L"       npu-broadcast run <input-index> <output-index> [seconds] [--room-echo|--bypass] [--loopback]\n"
               << L"       npu-broadcast test <input-index> <output-index> <seconds> [--room-echo|--bypass] [--loopback]\n"
               << L"       npu-broadcast route-probe <playback-index> <recording-index> <fixture.pcm> <result-base>\n";
    return 1;
  }
  try {
    ComApartment apartment;
    auto enumerator = device_enumerator();
    auto inputs = endpoints(enumerator.Get(), loopback ? eRender : eCapture);
    auto outputs = endpoints(enumerator.Get(), eRender);
    if (argc == 2 && std::wstring(argv[1]) == L"list") {
      list_devices(inputs.Get(), loopback ? L"Playback input" : L"Input");
      list_devices(outputs.Get(), L"Output");
      return 0;
    }
    const std::wstring command = argv[1];
    if (argc < 4 || (command != L"run" && (command != L"check" || argc != 4) &&
                     (command != L"test" || argc != 5))) {
      throw std::invalid_argument("Expected list, check, run, or test.");
    }
    const auto seconds = argc == 5 ? number(argv[4]) : 0;
    if (seconds > 3600 || (command == L"test" && seconds == 0)) {
      throw std::invalid_argument("Use a test duration from 1 to 3600 seconds.");
    }
    const auto input_index = number(argv[2]);
    const auto output_index = number(argv[3]);
    if (loopback && input_index == output_index) throw std::invalid_argument("Choose distinct playback input and output.");
    auto input = device_at(inputs.Get(), input_index);
    auto output = device_at(outputs.Get(), output_index);
    run(input.Get(), output.Get(), seconds, command == L"check", command == L"test", effect, loopback);
  } catch (const std::exception &error) {
    std::cerr << "NPU Broadcast: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
