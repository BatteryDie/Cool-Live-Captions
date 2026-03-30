#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <CoreAudio/CoreAudio.h>

class AudioMac {
public:
  using SampleHandler = std::function<void(const std::vector<float> &)>;
  struct AppInfo {
    uint32_t id = 0;
    std::string label;
  };

  void set_target_node(const std::optional<AppInfo> &app);
  std::vector<AppInfo> list_applications();
  bool start(size_t sample_rate, int /*source*/, SampleHandler handler);
  void stop();

private:
  SampleHandler handler_;
  size_t sample_rate_ = 0;
  int source_ = 0;
  std::atomic<bool> running_{false};
  std::mutex state_mutex_;

  AudioObjectID tap_id_ = kAudioObjectUnknown;
  AudioObjectID aggregate_device_id_ = kAudioObjectUnknown;
  AudioDeviceIOProcID io_proc_id_ = nullptr;
  AudioStreamBasicDescription tap_format_{};
  std::vector<float> resample_input_;
  double resample_pos_ = 0.0;

  std::optional<AppInfo> selected_app_;
};
