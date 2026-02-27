#include "audio_win.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <audiopolicy.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <processthreadsapi.h>
#include <propvarutil.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <Windows.h>
#include <wrl/client.h>
#include <wrl/ftm.h>
#include <wrl/implements.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {
void log_hr(const char *stage, HRESULT hr) {
  std::fprintf(stderr, "[error] WASAPI %s failed: 0x%08lx\n", stage, static_cast<unsigned long>(hr));
}

struct CoInitScope {
  HRESULT hr = E_FAIL;
  bool should_uninit = false;
  explicit CoInitScope(DWORD coinit) {
    hr = CoInitializeEx(nullptr, coinit);
    if (hr == S_OK) {
      should_uninit = true;
    }
  }
  ~CoInitScope() {
    if (should_uninit) {
      CoUninitialize();
    }
  }
};

std::string narrow_utf8(const std::wstring &w) {
  if (w.empty()) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string out;
  out.resize(static_cast<size_t>(size));
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring basename_w(const std::wstring &path) {
  if (path.empty()) {
    return {};
  }
  size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string process_label_from_pid(DWORD pid) {
  std::string label = "PID " + std::to_string(static_cast<unsigned long>(pid));
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) {
    return label;
  }
  wchar_t buf[MAX_PATH];
  DWORD size = static_cast<DWORD>(std::size(buf));
  if (QueryFullProcessImageNameW(h, 0, buf, &size) && size > 0) {
    std::wstring full(buf, buf + size);
    std::wstring base = basename_w(full);
    auto utf8 = narrow_utf8(base);
    if (!utf8.empty()) {
      label = utf8;
    }
  }
  CloseHandle(h);
  return label;
}

std::string process_basename_from_pid(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) {
    return {};
  }
  wchar_t buf[MAX_PATH];
  DWORD size = static_cast<DWORD>(std::size(buf));
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, buf, &size) && size > 0) {
    std::wstring full(buf, buf + size);
    out = narrow_utf8(basename_w(full));
  }
  CloseHandle(h);
  return out;
}

std::unordered_map<DWORD, DWORD> build_parent_pid_map() {
  std::unordered_map<DWORD, DWORD> map;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return map;
  }
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      map[entry.th32ProcessID] = entry.th32ParentProcessID;
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return map;
}

// Process loopback capture is only available on newer Windows SDKs.
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define COOL_CAPTIONS_HAVE_PROCESS_LOOPBACK 1
#else
#define COOL_CAPTIONS_HAVE_PROCESS_LOOPBACK 0
#endif

#if COOL_CAPTIONS_HAVE_PROCESS_LOOPBACK
class ActivateHandler final
  : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
              Microsoft::WRL::FtmBase,
              IActivateAudioInterfaceCompletionHandler> {
public:
  explicit ActivateHandler(HANDLE done) : done_(done) {}

  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override {
    HRESULT activate_hr = E_FAIL;
    ComPtr<IUnknown> unk;
    HRESULT hr = operation->GetActivateResult(&activate_hr, &unk);
    if (FAILED(hr)) {
      hr_ = hr;
    } else {
      hr_ = activate_hr;
      if (SUCCEEDED(hr_) && unk) {
        unk.As(&client_);
      }
    }
    SetEvent(done_);
    return S_OK;
  }

  HRESULT result() const { return hr_; }
  ComPtr<IAudioClient> client() const { return client_; }

private:
  HANDLE done_ = nullptr;
  HRESULT hr_ = E_FAIL;
  ComPtr<IAudioClient> client_;
};

ComPtr<IAudioClient> activate_process_loopback_client(DWORD pid) {
  HANDLE done = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!done) {
    std::fprintf(stderr, "[error] WASAPI CreateEvent failed (activate)\n");
    return nullptr;
  }

  auto handler = Make<ActivateHandler>(done);
  if (!handler) {
    CloseHandle(done);
    return nullptr;
  }

  // Allocate activation params as a BLOB.
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  __if_exists(AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS::TargetProcessId) {
    params.ProcessLoopbackParams.TargetProcessId = pid;
  }
  __if_not_exists(AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS::TargetProcessId) {
    __if_exists(AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS::ProcessId) {
      params.ProcessLoopbackParams.ProcessId = pid;
    }
    __if_not_exists(AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS::ProcessId) {
      std::fprintf(stderr, "[error] WASAPI process loopback params missing PID field in this SDK\n");
      CloseHandle(done);
      return nullptr;
    }
  }
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT pv;
  PropVariantInit(&pv);
  pv.vt = VT_BLOB;
  pv.blob.cbSize = static_cast<ULONG>(sizeof(params));
  pv.blob.pBlobData = static_cast<BYTE *>(CoTaskMemAlloc(sizeof(params)));
  if (!pv.blob.pBlobData) {
    CloseHandle(done);
    return nullptr;
  }
  std::memcpy(pv.blob.pBlobData, &params, sizeof(params));

  ComPtr<IActivateAudioInterfaceAsyncOperation> op;
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                           __uuidof(IAudioClient),
                                           &pv,
                                           handler.Get(),
                                           &op);

  // Free activation blob memory after starting the operation.
  CoTaskMemFree(pv.blob.pBlobData);
  pv.blob.pBlobData = nullptr;
  PropVariantClear(&pv);

  if (FAILED(hr)) {
    log_hr("ActivateAudioInterfaceAsync", hr);
    CloseHandle(done);
    return nullptr;
  }

  WaitForSingleObject(done, 5000);
  CloseHandle(done);

  if (FAILED(handler->result())) {
    log_hr("ProcessLoopback ActivateCompleted", handler->result());
    return nullptr;
  }
  return handler->client();
}
#endif
}

void AudioWin::set_target_node(const std::optional<AppInfo> &app) {
  if (app) {
    target_pid_ = app->id;
  } else {
    target_pid_.reset();
  }
}

std::vector<AudioWin::AppInfo> AudioWin::list_applications() {
  std::vector<AppInfo> out;

  CoInitScope co(COINIT_MULTITHREADED);
  // If COM is already initialized in another mode (e.g., STA), proceed anyway.
  if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE) {
    log_hr("CoInitializeEx(list_applications)", co.hr);
    return out;
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    log_hr("CoCreateInstance(MMDeviceEnumerator)", hr);
    return out;
  }

  ComPtr<IMMDevice> device;
  const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
  bool got_device = false;
  for (ERole role : roles) {
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) {
      got_device = true;
      break;
    }
  }
  if (!got_device) {
    return out;
  }

  ComPtr<IAudioSessionManager2> mgr;
  hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(mgr.GetAddressOf()));
  if (FAILED(hr) || !mgr) {
    log_hr("Activate(IAudioSessionManager2)", hr);
    return out;
  }

  ComPtr<IAudioSessionEnumerator> sessions;
  hr = mgr->GetSessionEnumerator(&sessions);
  if (FAILED(hr) || !sessions) {
    log_hr("GetSessionEnumerator", hr);
    return out;
  }

  int count = 0;
  hr = sessions->GetCount(&count);
  if (FAILED(hr) || count <= 0) {
    return out;
  }

  auto parent_map = build_parent_pid_map();
  std::unordered_map<DWORD, std::string> base_cache;
  base_cache.reserve(static_cast<size_t>(count));

  auto get_base = [&](DWORD pid) -> std::string {
    auto it = base_cache.find(pid);
    if (it != base_cache.end()) {
      return it->second;
    }
    std::string base = process_basename_from_pid(pid);
    base_cache.emplace(pid, base);
    return base;
  };

  auto get_parent = [&](DWORD pid) -> DWORD {
    auto it = parent_map.find(pid);
    if (it == parent_map.end()) {
      return 0;
    }
    return it->second;
  };

  auto root_same_image = [&](DWORD pid) -> DWORD {
    std::string base = get_base(pid);
    if (base.empty()) {
      return pid;
    }
    DWORD cur = pid;
    while (true) {
      DWORD parent = get_parent(cur);
      if (!parent || parent == cur) {
        break;
      }
      std::string parent_base = get_base(parent);
      if (parent_base.empty() || parent_base != base) {
        break;
      }
      cur = parent;
    }
    return cur;
  };

  std::vector<DWORD> seen;
  seen.reserve(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i) {
    ComPtr<IAudioSessionControl> ctl;
    if (FAILED(sessions->GetSession(i, &ctl)) || !ctl) {
      continue;
    }

    ComPtr<IAudioSessionControl2> ctl2;
    if (FAILED(ctl.As(&ctl2)) || !ctl2) {
      continue;
    }

    DWORD pid = 0;
    if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0) {
      continue;
    }

    DWORD target_pid = root_same_image(pid);
    if (std::find(seen.begin(), seen.end(), target_pid) != seen.end()) {
      continue;
    }

    std::string label = get_base(target_pid);
    if (label.empty()) {
      label = process_label_from_pid(target_pid);
    }

    AppInfo info;
    info.id = static_cast<uint32_t>(target_pid);
    info.label = std::move(label);
    out.push_back(std::move(info));
    seen.push_back(target_pid);
  }

  std::sort(out.begin(), out.end(), [](const AppInfo &a, const AppInfo &b) {
    return a.label < b.label;
  });
  return out;
}

bool AudioWin::start(size_t sample_rate, Source source, SampleHandler handler) {
  if (sample_rate == 0) {
    return false;
  }
  if (running_) {
    return true;
  }
  sample_rate_ = sample_rate;
  source_ = source;
  handler_ = std::move(handler);
  running_ = true;
  worker_ = std::thread(&AudioWin::run_loop, this);
  return true;
}

void AudioWin::stop() {
  running_ = false;
  if (client_) {
    client_->Stop();
  }
  if (capture_event_) {
    SetEvent(capture_event_);
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  client_.Reset();
  if (capture_event_) {
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
  }
}

bool AudioWin::running() const {
  return running_.load();
}

void AudioWin::run_loop() {
  CoInitScope co(COINIT_MULTITHREADED);
  if (FAILED(co.hr)) {
    log_hr("CoInitializeEx", co.hr);
    running_ = false;
    return;
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    log_hr("CoCreateInstance(MMDeviceEnumerator)", hr);
    running_ = false;
    return;
  }

  const bool want_loopback = source_ == Source::Loopback || source_ == Source::Application;
  EDataFlow flow = want_loopback ? eRender : eCapture;
  const char *source_label = source_ == Source::Loopback ? "loopback" : (source_ == Source::Microphone ? "microphone" : "application");

  ComPtr<IAudioClient> client;
  if (source_ == Source::Application) {
#if COOL_CAPTIONS_HAVE_PROCESS_LOOPBACK
    if (!target_pid_) {
      std::fprintf(stderr, "[error] WASAPI no target PID set for application capture\n");
      running_ = false;
      return;
    }
    client = activate_process_loopback_client(static_cast<DWORD>(*target_pid_));
    if (!client) {
      std::fprintf(stderr, "[error] WASAPI process loopback activation failed (pid=%lu)\n",
                   static_cast<unsigned long>(*target_pid_));
      running_ = false;
      return;
    }
#else
    std::fprintf(stderr, "[error] WASAPI process loopback not supported by this Windows SDK build\n");
    running_ = false;
    return;
#endif
  } else {
    ComPtr<IMMDevice> device;
    const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
    bool got_device = false;
    for (ERole role : roles) {
      if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) {
        got_device = true;
        break;
      }
    }
    if (!got_device) {
      std::fprintf(stderr, "[error] WASAPI no default %s device found\n", source_label);
      running_ = false;
      return;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf()));
    if (FAILED(hr)) {
      log_hr("Activate(IAudioClient)", hr);
      running_ = false;
      return;
    }
  }
  client_ = client;

  // For process-loopback virtual clients, GetMixFormat can return E_NOTIMPL.
  // Use the default render endpoint mix format instead.
  WAVEFORMATEX *mix_raw = nullptr;
  if (source_ == Source::Application) {
    ComPtr<IMMDevice> mix_device;
    const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
    bool got_device = false;
    for (ERole role : roles) {
      if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role, &mix_device))) {
        got_device = true;
        break;
      }
    }
    if (!got_device) {
      std::fprintf(stderr, "[error] WASAPI no default render device found for mix format\n");
      running_ = false;
      return;
    }
    ComPtr<IAudioClient> mix_client;
    hr = mix_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(mix_client.GetAddressOf()));
    if (FAILED(hr)) {
      log_hr("Activate(IAudioClient) for mix", hr);
      running_ = false;
      return;
    }
    hr = mix_client->GetMixFormat(&mix_raw);
  } else {
    hr = client->GetMixFormat(&mix_raw);
  }
  if (FAILED(hr) || !mix_raw) {
    log_hr("GetMixFormat", hr);
    running_ = false;
    return;
  }
  std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(mix_raw, &CoTaskMemFree);

  const bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix.get())->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  const bool is_pcm16 = mix->wFormatTag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16;
  const bool is_ext16 = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix.get())->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
                        mix->wBitsPerSample == 16;
  const WORD channels = mix->nChannels;
  const UINT32 mix_rate = mix->nSamplesPerSec;
  if (channels == 0) {
    running_ = false;
    return;
  }

  const REFERENCE_TIME hns_buffer = 100 * 10000; // 100 ms
  DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (want_loopback) {
    flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, hns_buffer, 0, mix.get(), nullptr);
  if (FAILED(hr)) {
    log_hr("Initialize", hr);
    running_ = false;
    return;
  }

  ComPtr<IAudioCaptureClient> capture;
  hr = client->GetService(IID_PPV_ARGS(&capture));
  if (FAILED(hr)) {
    log_hr("GetService(IAudioCaptureClient)", hr);
    running_ = false;
    return;
  }

  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!event) {
    std::fprintf(stderr, "[error] WASAPI CreateEvent failed\n");
    running_ = false;
    return;
  }
  hr = client->SetEventHandle(event);
  if (FAILED(hr)) {
    log_hr("SetEventHandle", hr);
    CloseHandle(event);
    running_ = false;
    return;
  }
  capture_event_ = event;

  hr = client->Start();
  if (FAILED(hr)) {
    log_hr("Start", hr);
    running_ = false;
    return;
  }

  std::fprintf(stdout, "[info] WASAPI started (%s, mix %u Hz, %u ch -> %zu Hz)\n", source_label,
               mix_rate, static_cast<unsigned int>(channels), sample_rate_);

  const double step = static_cast<double>(mix_rate) / static_cast<double>(sample_rate_);

  std::vector<float> buffer;
  std::vector<float> mono;
  bool logged_first_packet = false;

  while (running_) {
    DWORD wait = WaitForSingleObject(capture_event_, 200);
    if (wait != WAIT_OBJECT_0) {
      continue;
    }

    UINT32 packet = 0;
    if (FAILED(capture->GetNextPacketSize(&packet))) {
      break;
    }
    if (packet == 0) {
      continue;
    }

    BYTE *data = nullptr;
    UINT32 frames = 0;
    DWORD capture_flags = 0;
    hr = capture->GetBuffer(&data, &frames, &capture_flags, nullptr, nullptr);
    if (FAILED(hr)) {
      break;
    }

    const size_t samples = static_cast<size_t>(frames) * channels;
    buffer.resize(samples);

    const bool silent = (capture_flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr;
    if (silent) {
      std::fill(buffer.begin(), buffer.end(), 0.0f);
    } else if (is_float) {
      const float *src = reinterpret_cast<const float *>(data);
      std::copy(src, src + samples, buffer.begin());
    } else if (is_pcm16 || is_ext16) {
      const int16_t *src = reinterpret_cast<const int16_t *>(data);
      constexpr float scale = 1.0f / 32768.0f;
      for (size_t i = 0; i < samples; ++i) {
        buffer[i] = static_cast<float>(src[i]) * scale;
      }
    } else {
      std::fill(buffer.begin(), buffer.end(), 0.0f);
    }

    mono.resize(frames);
    for (UINT32 i = 0; i < frames; ++i) {
      float sum = 0.0f;
      for (UINT32 ch = 0; ch < channels; ++ch) {
        sum += buffer[i * channels + ch];
      }
      mono[i] = sum / static_cast<float>(channels);
    }

    std::vector<float> output;
    if (sample_rate_ == mix_rate) {
      output = mono;
    } else {
      const size_t out_frames = std::max<size_t>(1, static_cast<size_t>(std::round(mono.size() / step)));
      output.resize(out_frames);
      for (size_t i = 0; i < out_frames; ++i) {
        double src_pos = static_cast<double>(i) * step;
        size_t idx = static_cast<size_t>(src_pos);
        double frac = src_pos - static_cast<double>(idx);
        float s0 = mono[std::min(idx, mono.size() - 1)];
        float s1 = mono[std::min(idx + 1, mono.size() - 1)];
        output[i] = s0 + static_cast<float>(frac) * (s1 - s0);
      }
    }

    if (!silent && !logged_first_packet) {
      std::fprintf(stdout, "[info] WASAPI captured first packet (%u frames)\n", frames);
      logged_first_packet = true;
    }

    if (handler_ && !output.empty()) {
      handler_(output);
    }

    capture->ReleaseBuffer(frames);
  }

  client->Stop();
  client_.Reset();
  if (capture_event_) {
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
  }
}
