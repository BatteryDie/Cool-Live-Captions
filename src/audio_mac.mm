#include "audio_mac.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <unistd.h>

#include <AppKit/AppKit.h>
#include <CoreAudio/CATapDescription.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareTapping.h>

namespace {

bool read_property(AudioObjectID object_id,
                   AudioObjectPropertySelector selector,
                   AudioObjectPropertyScope scope,
                   AudioObjectPropertyElement element,
                   void *data,
                   UInt32 data_size,
                   const void *qualifier_data = nullptr,
                   UInt32 qualifier_size = 0) {
  AudioObjectPropertyAddress address{selector, scope, element};
  UInt32 io_size = data_size;
  return AudioObjectGetPropertyData(object_id,
                                    &address,
                                    qualifier_size,
                                    qualifier_data,
                                    &io_size,
                                    data) == noErr &&
         io_size == data_size;
}

bool read_default_output_device(AudioObjectID *out_device_id) {
  if (!out_device_id) {
    return false;
  }
  return read_property(kAudioObjectSystemObject,
                       kAudioHardwarePropertyDefaultSystemOutputDevice,
                       kAudioObjectPropertyScopeGlobal,
                       kAudioObjectPropertyElementMain,
                       out_device_id,
                       sizeof(*out_device_id));
}

bool read_device_uid(AudioObjectID device_id, NSString **out_uid) {
  if (!out_uid) {
    return false;
  }
  CFStringRef uid_ref = nullptr;
  if (!read_property(device_id,
                     kAudioDevicePropertyDeviceUID,
                     kAudioObjectPropertyScopeGlobal,
                     kAudioObjectPropertyElementMain,
                     &uid_ref,
                     sizeof(uid_ref))) {
    return false;
  }
  if (!uid_ref) {
    return false;
  }
  NSString *uid = [(__bridge NSString *)uid_ref copy];
  CFRelease(uid_ref);
  *out_uid = uid;
  return *out_uid != nil;
}

bool translate_pid_to_process_object(pid_t pid, AudioObjectID *out_process_object_id) {
  if (!out_process_object_id) {
    return false;
  }
  return read_property(kAudioObjectSystemObject,
                       kAudioHardwarePropertyTranslatePIDToProcessObject,
                       kAudioObjectPropertyScopeGlobal,
                       kAudioObjectPropertyElementMain,
                       out_process_object_id,
                       sizeof(*out_process_object_id),
                       &pid,
                       sizeof(pid));
}

bool read_tap_format(AudioObjectID tap_id, AudioStreamBasicDescription *out_format) {
  if (!out_format) {
    return false;
  }
  return read_property(tap_id,
                       kAudioTapPropertyFormat,
                       kAudioObjectPropertyScopeGlobal,
                       kAudioObjectPropertyElementMain,
                       out_format,
                       sizeof(*out_format));
}

std::string app_label_for_pid(pid_t pid, NSString *bundle_id) {
  NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
  if (app) {
    NSString *name = app.localizedName;
    if (name.length > 0) {
      return std::string(name.UTF8String);
    }
  }

  if (bundle_id && bundle_id.length > 0) {
    return std::string(bundle_id.UTF8String);
  }

  return "App " + std::to_string(static_cast<uint32_t>(pid));
}

} // namespace

void AudioMac::set_target_node(const std::optional<AppInfo> &app) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  selected_app_ = app;
}

std::vector<AudioMac::AppInfo> AudioMac::list_applications() {
  std::vector<AppInfo> apps;

  AudioObjectPropertyAddress process_list_addr{
      kAudioHardwarePropertyProcessObjectList,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain};

  UInt32 data_size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                     &process_list_addr,
                                     0,
                                     nullptr,
                                     &data_size) != noErr ||
      data_size == 0) {
    return apps;
  }

  std::vector<AudioObjectID> process_objects(data_size / sizeof(AudioObjectID), kAudioObjectUnknown);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                 &process_list_addr,
                                 0,
                                 nullptr,
                                 &data_size,
                                 process_objects.data()) != noErr) {
    return apps;
  }

  std::unordered_set<uint32_t> seen;
  seen.reserve(process_objects.size());

  for (AudioObjectID process_object_id : process_objects) {
    if (process_object_id == kAudioObjectUnknown) {
      continue;
    }

    pid_t pid = 0;
    if (!read_property(process_object_id,
                       kAudioProcessPropertyPID,
                       kAudioObjectPropertyScopeGlobal,
                       kAudioObjectPropertyElementMain,
                       &pid,
                       sizeof(pid))) {
      continue;
    }
    if (pid <= 0 || pid == getpid()) {
      continue;
    }

    UInt32 is_running_output = 0;
    if (!read_property(process_object_id,
                       kAudioProcessPropertyIsRunningOutput,
                       kAudioObjectPropertyScopeGlobal,
                       kAudioObjectPropertyElementMain,
                       &is_running_output,
                       sizeof(is_running_output)) ||
        is_running_output == 0) {
      continue;
    }

    if (!seen.insert(static_cast<uint32_t>(pid)).second) {
      continue;
    }

    CFStringRef bundle_id_ref = nullptr;
    NSString *bundle_id = nil;
    if (read_property(process_object_id,
                      kAudioProcessPropertyBundleID,
                      kAudioObjectPropertyScopeGlobal,
                      kAudioObjectPropertyElementMain,
                      &bundle_id_ref,
                      sizeof(bundle_id_ref)) &&
        bundle_id_ref != nullptr) {
      bundle_id = [(__bridge NSString *)bundle_id_ref copy];
      CFRelease(bundle_id_ref);
    }

    AppInfo info;
    info.id = static_cast<uint32_t>(pid);
    info.label = app_label_for_pid(pid, bundle_id);
    if (!info.label.empty()) {
      apps.push_back(std::move(info));
    }
  }

  std::sort(apps.begin(), apps.end(), [](const AppInfo &a, const AppInfo &b) {
    if (a.label == b.label) {
      return a.id < b.id;
    }
    return a.label < b.label;
  });

  return apps;
}

bool AudioMac::start(size_t sample_rate, int source, SampleHandler handler) {
  if (sample_rate == 0 || !handler) {
    return false;
  }

  stop();

  AppInfo selected;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    sample_rate_ = sample_rate;
    source_ = source;
    handler_ = std::move(handler);
    resample_input_.clear();
    resample_pos_ = 0.0;

    if (source_ != 2 || !selected_app_.has_value()) {
      handler_ = nullptr;
      return false;
    }
    selected = *selected_app_;
  }

  const pid_t target_pid = static_cast<pid_t>(selected.id);
  if (target_pid <= 0) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  AudioObjectID process_object_id = kAudioObjectUnknown;
  if (!translate_pid_to_process_object(target_pid, &process_object_id) ||
      process_object_id == kAudioObjectUnknown) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  CATapDescription *tap_desc = [[CATapDescription alloc] initStereoMixdownOfProcesses:@[@(process_object_id)]];
  if (!tap_desc) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  tap_desc.UUID = [NSUUID UUID];
  tap_desc.privateTap = YES;
  tap_desc.muteBehavior = CATapUnmuted;

  AudioObjectID tap_id = kAudioObjectUnknown;
  if (AudioHardwareCreateProcessTap(tap_desc, &tap_id) != noErr || tap_id == kAudioObjectUnknown) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  AudioObjectID output_device_id = kAudioObjectUnknown;
  NSString *output_uid = nil;
  if (!read_default_output_device(&output_device_id) ||
      output_device_id == kAudioObjectUnknown ||
      !read_device_uid(output_device_id, &output_uid) ||
      output_uid.length == 0) {
    AudioHardwareDestroyProcessTap(tap_id);
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  AudioStreamBasicDescription tap_format{};
  if (!read_tap_format(tap_id, &tap_format)) {
    AudioHardwareDestroyProcessTap(tap_id);
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  NSString *aggregate_uid = NSUUID.UUID.UUIDString;
  NSDictionary *aggregate_description = @{
    @kAudioAggregateDeviceNameKey : [NSString stringWithFormat:@"CLC Tap %u", selected.id],
    @kAudioAggregateDeviceUIDKey : aggregate_uid,
    @kAudioAggregateDeviceMainSubDeviceKey : output_uid,
    @kAudioAggregateDeviceIsPrivateKey : @YES,
    @kAudioAggregateDeviceTapAutoStartKey : @YES,
    @kAudioAggregateDeviceSubDeviceListKey : @[
      @{ @kAudioSubDeviceUIDKey : output_uid }
    ],
    @kAudioAggregateDeviceTapListKey : @[
      @{
        @kAudioSubTapUIDKey : tap_desc.UUID.UUIDString,
        @kAudioSubTapDriftCompensationKey : @YES,
      }
    ],
  };

  AudioObjectID aggregate_device_id = kAudioObjectUnknown;
  if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate_description,
                                         &aggregate_device_id) != noErr ||
      aggregate_device_id == kAudioObjectUnknown) {
    AudioHardwareDestroyProcessTap(tap_id);
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  AudioDeviceIOProcID io_proc_id = nullptr;
  auto *self = this;
  OSStatus status = AudioDeviceCreateIOProcIDWithBlock(
      &io_proc_id,
      aggregate_device_id,
      nullptr,
      ^(const AudioTimeStamp *inNow,
        const AudioBufferList *inInputData,
        const AudioTimeStamp *inInputTime,
        AudioBufferList *outOutputData,
        const AudioTimeStamp *inOutputTime) {
        (void)inNow;
        (void)inInputTime;
        (void)outOutputData;
        (void)inOutputTime;

        if (!self || !inInputData) {
          return;
        }

        AudioMac::SampleHandler handler_copy;
        AudioStreamBasicDescription fmt{};
        size_t target_sample_rate = 0;
        {
          std::lock_guard<std::mutex> lock(self->state_mutex_);
          if (!self->running_ || !self->handler_) {
            return;
          }
          handler_copy = self->handler_;
          fmt = self->tap_format_;
          target_sample_rate = self->sample_rate_;
        }

        if ((fmt.mFormatFlags & kAudioFormatFlagIsFloat) == 0 || fmt.mBitsPerChannel != 32) {
          return;
        }

        const UInt32 num_buffers = inInputData->mNumberBuffers;
        if (num_buffers == 0) {
          return;
        }

        std::vector<float> mono;

        if (num_buffers == 1) {
          const AudioBuffer &buffer = inInputData->mBuffers[0];
          if (!buffer.mData || buffer.mDataByteSize < sizeof(float)) {
            return;
          }
          UInt32 channels = fmt.mChannelsPerFrame > 0 ? fmt.mChannelsPerFrame : 1;
          const UInt32 total_samples = buffer.mDataByteSize / static_cast<UInt32>(sizeof(float));
          const UInt32 frames = channels > 0 ? total_samples / channels : 0;
          if (frames == 0) {
            return;
          }

          const float *data = static_cast<const float *>(buffer.mData);
          mono.resize(frames);
          for (UInt32 frame = 0; frame < frames; ++frame) {
            float sum = 0.0f;
            for (UInt32 channel = 0; channel < channels; ++channel) {
              sum += data[frame * channels + channel];
            }
            mono[frame] = sum / static_cast<float>(channels);
          }
        } else {
          const UInt32 channels = num_buffers;
          UInt32 frames = std::numeric_limits<UInt32>::max();
          for (UInt32 channel = 0; channel < channels; ++channel) {
            const AudioBuffer &buffer = inInputData->mBuffers[channel];
            if (!buffer.mData || buffer.mDataByteSize < sizeof(float)) {
              return;
            }
            frames = std::min(frames, buffer.mDataByteSize / static_cast<UInt32>(sizeof(float)));
          }
          if (frames == 0 || frames == std::numeric_limits<UInt32>::max()) {
            return;
          }

          mono.resize(frames);
          for (UInt32 frame = 0; frame < frames; ++frame) {
            float sum = 0.0f;
            for (UInt32 channel = 0; channel < channels; ++channel) {
              const float *channel_data = static_cast<const float *>(inInputData->mBuffers[channel].mData);
              sum += channel_data[frame];
            }
            mono[frame] = sum / static_cast<float>(channels);
          }
        }

        if (mono.empty()) {
          return;
        }

        std::vector<float> output = std::move(mono);

        if (target_sample_rate > 0 && fmt.mSampleRate > 0.0 &&
            std::fabs(fmt.mSampleRate - static_cast<double>(target_sample_rate)) > 1.0) {
          std::lock_guard<std::mutex> lock(self->state_mutex_);
          if (!self->running_) {
            return;
          }

          self->resample_input_.insert(self->resample_input_.end(), output.begin(), output.end());
          output.clear();

          const double step = fmt.mSampleRate / static_cast<double>(target_sample_rate);
          while (self->resample_pos_ + 1.0 < static_cast<double>(self->resample_input_.size())) {
            const size_t i0 = static_cast<size_t>(self->resample_pos_);
            const size_t i1 = i0 + 1;
            const float s0 = self->resample_input_[i0];
            const float s1 = self->resample_input_[i1];
            const double frac = self->resample_pos_ - static_cast<double>(i0);
            output.push_back(static_cast<float>(s0 + (s1 - s0) * frac));
            self->resample_pos_ += step;
          }

          const size_t consumed = static_cast<size_t>(self->resample_pos_);
          if (consumed > 0 && consumed <= self->resample_input_.size()) {
            self->resample_input_.erase(self->resample_input_.begin(), self->resample_input_.begin() + consumed);
            self->resample_pos_ -= static_cast<double>(consumed);
          }
        }

        if (!output.empty()) {
          handler_copy(output);
        }
      });

  if (status != noErr || io_proc_id == nullptr) {
    AudioHardwareDestroyAggregateDevice(aggregate_device_id);
    AudioHardwareDestroyProcessTap(tap_id);
    std::lock_guard<std::mutex> lock(state_mutex_);
    handler_ = nullptr;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    tap_id_ = tap_id;
    aggregate_device_id_ = aggregate_device_id;
    io_proc_id_ = io_proc_id;
    tap_format_ = tap_format;
    running_ = true;
  }

  status = AudioDeviceStart(aggregate_device_id, io_proc_id);
  if (status != noErr) {
    stop();
    return false;
  }

  return true;
}

void AudioMac::stop() {
  AudioObjectID aggregate_device_id = kAudioObjectUnknown;
  AudioObjectID tap_id = kAudioObjectUnknown;
  AudioDeviceIOProcID io_proc_id = nullptr;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    running_ = false;

    aggregate_device_id = aggregate_device_id_;
    tap_id = tap_id_;
    io_proc_id = io_proc_id_;

    aggregate_device_id_ = kAudioObjectUnknown;
    tap_id_ = kAudioObjectUnknown;
    io_proc_id_ = nullptr;
    tap_format_ = AudioStreamBasicDescription{};
    handler_ = nullptr;
    resample_input_.clear();
    resample_pos_ = 0.0;
  }

  if (aggregate_device_id != kAudioObjectUnknown && io_proc_id != nullptr) {
    AudioDeviceStop(aggregate_device_id, io_proc_id);
    AudioDeviceDestroyIOProcID(aggregate_device_id, io_proc_id);
  }

  if (aggregate_device_id != kAudioObjectUnknown) {
    AudioHardwareDestroyAggregateDevice(aggregate_device_id);
  }

  if (tap_id != kAudioObjectUnknown) {
    AudioHardwareDestroyProcessTap(tap_id);
  }
}
