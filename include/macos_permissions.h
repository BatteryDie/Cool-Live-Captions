#pragma once

#ifdef __APPLE__

#include <functional>

enum class MacPermissionStatus {
    Granted,
    Denied,
    NotDetermined,
    Restricted,
    Unavailable,
};

struct MacPermissionState {
    MacPermissionStatus file_folder          = MacPermissionStatus::NotDetermined;
    MacPermissionStatus microphone           = MacPermissionStatus::NotDetermined;
    MacPermissionStatus system_audio_only    = MacPermissionStatus::Unavailable;
};

// Synchronous status check; never prompts the user.
MacPermissionState macos_check_permissions();

// Completion runs on the main thread.
void macos_request_microphone(std::function<void(bool)> completion);

// Completion runs on the main thread.
void macos_request_screen_capture(std::function<void(bool)> completion);

// Uses private TCC SPI (kTCCServiceAudioCapture) when available.
// Completion runs on the main thread.
void macos_request_audio_capture(std::function<void(bool)> completion);

void macos_open_screen_capture_settings();

void macos_open_audio_capture_settings();

void macos_open_files_settings();

// Audio-only system capture support requires macOS 14.4+.
bool macos_system_audio_only_supported();

#endif // __APPLE__
