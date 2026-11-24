#pragma once

// Shared process-wide state for the XAML window layer.
namespace npu {

// Set by show_window before the XAML application starts. The smoke test
// closes the window shortly after launch so automated checks stay fast.
inline bool& smoke_test() {
  static bool value = false;
  return value;
}

}  // namespace npu
