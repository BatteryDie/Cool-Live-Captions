#include "macos_permissions.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>

static MacPermissionStatus av_status_to_mac(AVAuthorizationStatus status) {
    switch (status) {
        case AVAuthorizationStatusAuthorized:    return MacPermissionStatus::Granted;
        case AVAuthorizationStatusDenied:        return MacPermissionStatus::Denied;
        case AVAuthorizationStatusRestricted:    return MacPermissionStatus::Restricted;
        case AVAuthorizationStatusNotDetermined: return MacPermissionStatus::NotDetermined;
        default:                                 return MacPermissionStatus::NotDetermined;
    }
}

static MacPermissionStatus check_file_folder() {
    NSString *homePath = NSHomeDirectory();
    if (!homePath) {
        return MacPermissionStatus::Denied;
    }
    BOOL readable = [[NSFileManager defaultManager] isReadableFileAtPath:homePath];
    return readable ? MacPermissionStatus::Granted : MacPermissionStatus::Denied;
}

static MacPermissionStatus check_microphone() {
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    return av_status_to_mac(status);
}

using TCCAccessPreflightFn = int (*)(CFStringRef, CFDictionaryRef);
using TCCAccessRequestFn = void (*)(CFStringRef, CFDictionaryRef, void (^)(Boolean));

// TCCAccessPreflight/TCCAccessRequest are private SPI resolved at runtime.

static void *tcc_handle() {
    static void *handle = dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC", RTLD_NOW);
    return handle;
}

static TCCAccessPreflightFn tcc_preflight_fn() {
    static TCCAccessPreflightFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (void *handle = tcc_handle()) {
            if (void *sym = dlsym(handle, "TCCAccessPreflight")) {
                fn = reinterpret_cast<TCCAccessPreflightFn>(sym);
            }
        }
    }
    return fn;
}

static TCCAccessRequestFn tcc_request_fn() {
    static TCCAccessRequestFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (void *handle = tcc_handle()) {
            if (void *sym = dlsym(handle, "TCCAccessRequest")) {
                fn = reinterpret_cast<TCCAccessRequestFn>(sym);
            }
        }
    }
    return fn;
}

static MacPermissionStatus check_audio_capture_only() {
    if (!macos_system_audio_only_supported()) {
        return MacPermissionStatus::Unavailable;
    }

    if (TCCAccessPreflightFn preflight = tcc_preflight_fn()) {
        const int result = preflight(CFSTR("kTCCServiceAudioCapture"), nullptr);
        if (result == 0) {
            return MacPermissionStatus::Granted;
        }
        if (result == 1) {
            return MacPermissionStatus::Denied;
        }
        return MacPermissionStatus::NotDetermined;
    }

    return MacPermissionStatus::NotDetermined;
}

static MacPermissionStatus check_screen_capture_only() {
    if (TCCAccessPreflightFn preflight = tcc_preflight_fn()) {
        const int result = preflight(CFSTR("kTCCServiceScreenCapture"), nullptr);
        if (result == 0) {
            return MacPermissionStatus::Granted;
        }
        if (result == 1) {
            return MacPermissionStatus::Denied;
        }
        return MacPermissionStatus::NotDetermined;
    }

    return CGPreflightScreenCaptureAccess() ? MacPermissionStatus::Granted : MacPermissionStatus::Denied;
}

static MacPermissionStatus check_system_audio_only() {
    return check_audio_capture_only();
}

MacPermissionState macos_check_permissions() {
    MacPermissionState state;
    state.file_folder         = check_file_folder();
    state.microphone          = check_microphone();
    state.system_audio_only   = check_system_audio_only();
    return state;
}

void macos_request_microphone(std::function<void(bool)> completion) {
    AVAuthorizationStatus current =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (current != AVAuthorizationStatusNotDetermined) {
        if (completion) {
            completion(current == AVAuthorizationStatusAuthorized);
        }
        return;
    }

    auto completion_copy = completion;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                            completionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion_copy) {
                completion_copy(granted == YES);
            }
        });
    }];
}

void macos_request_screen_capture(std::function<void(bool)> completion) {
    if (check_screen_capture_only() == MacPermissionStatus::Granted) {
        if (completion) {
            completion(true);
        }
        return;
    }

    if (TCCAccessRequestFn request = tcc_request_fn()) {
        auto completion_copy = completion;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            request(CFSTR("kTCCServiceScreenCapture"), nullptr, ^(Boolean granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (completion_copy) {
                        completion_copy(granted == true);
                    }
                });
            });
        });
        return;
    }

    // This can block while the user responds to the system prompt.
    auto completion_copy = completion;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL granted = CGRequestScreenCaptureAccess();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion_copy) {
                completion_copy(granted == YES);
            }
        });
    });
}

void macos_request_audio_capture(std::function<void(bool)> completion) {
    if (check_audio_capture_only() == MacPermissionStatus::Granted) {
        if (completion) {
            completion(true);
        }
        return;
    }

    TCCAccessRequestFn request = tcc_request_fn();
    if (!request) {
        if (completion) {
            completion(false);
        }
        return;
    }

    auto completion_copy = completion;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        request(CFSTR("kTCCServiceAudioCapture"), nullptr, ^(Boolean granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion_copy) {
                    completion_copy(granted == true);
                }
            });
        });
    });
}

void macos_open_screen_capture_settings() {
    NSURL *url = nil;
    if (@available(macOS 13.0, *)) {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    } else {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security"];
    }
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

void macos_open_audio_capture_settings() {
    NSURL *url = nil;
    if (@available(macOS 13.0, *)) {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture"];
    } else {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security"];
    }
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

void macos_open_files_settings() {
    NSURL *url = nil;
    if (@available(macOS 13.0, *)) {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"];
    } else {
        url = [NSURL URLWithString:
               @"x-apple.systempreferences:com.apple.preference.security"];
    }
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

bool macos_system_audio_only_supported() {
    // App-audio capture permission is only available on macOS 14.4+.
    if (@available(macOS 14.4, *)) {
        return true;
    }
    return false;
}
