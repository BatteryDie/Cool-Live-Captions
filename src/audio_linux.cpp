#include "audio_linux.h"

#ifdef HAVE_PIPEWIRE

#include <algorithm>
#include <string_view>

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/utils/hook.h>

namespace {
const int kChannels = 2;

struct AppListState {
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook registry_listener{};
  spa_hook core_listener{};
  std::vector<AudioLinux::AppInfo> apps;
  int sync_seq = 0;
  bool done = false;
};

bool is_output_stream(const spa_dict *props) {
  if (!props) {
    return false;
  }
  const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!media_class) {
    return false;
  }
  return std::string_view(media_class) == "Stream/Output/Audio";
}

std::string make_app_label(const spa_dict *props, uint32_t id) {
  const char *app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
  if (!app_name || !*app_name) {
    app_name = spa_dict_lookup(props, "application.name");
  }
  const char *media_name = spa_dict_lookup(props, PW_KEY_MEDIA_NAME);
  const char *node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

  std::string label;
  if (app_name && *app_name) {
    label = app_name;
  } else if (node_desc && *node_desc) {
    label = node_desc;
  } else if (media_name && *media_name) {
    label = media_name;
  } else if (node_name && *node_name) {
    label = node_name;
  } else {
    label = "App " + std::to_string(id);
  }

  if (app_name && *app_name && media_name && *media_name && label == app_name) {
    if (std::string_view(media_name) != std::string_view(app_name)) {
      label += " - ";
      label += media_name;
    }
  }

  return label;
}

void on_registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                        uint32_t version, const spa_dict *props) {
  (void)permissions;
  (void)version;
  auto *state = static_cast<AppListState *>(data);
  if (!state || !type || std::string_view(type) != PW_TYPE_INTERFACE_Node) {
    return;
  }
  if (!is_output_stream(props)) {
    return;
  }
  AudioLinux::AppInfo info;
  info.id = id;
  info.label = make_app_label(props, id);
  if (const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME); node_name && *node_name) {
    info.node_name = node_name;
  }
  if (!info.label.empty()) {
    state->apps.push_back(std::move(info));
  }
}

void on_core_done(void *data, uint32_t id, int seq) {
  (void)id;
  auto *state = static_cast<AppListState *>(data);
  if (!state || seq != state->sync_seq) {
    return;
  }
  state->done = true;
  pw_thread_loop_signal(state->loop, false);
}
}

void AudioLinux::ensure_init() {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

bool AudioLinux::start(size_t sample_rate, int source, SampleHandler handler) {
  if (sample_rate == 0 || !handler) {
    return false;
  }
  if (running_) {
    return true;
  }

  ensure_init();

  handler_ = std::move(handler);
  sample_rate_ = sample_rate;
  source_ = source;

  loop_ = pw_thread_loop_new("coollivecaptions-audio", nullptr);
  if (!loop_) {
    return false;
  }

  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  struct pw_core *core = pw_context_connect(context_, nullptr, 0);
  if (!core) {
    pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
    context_ = nullptr;
    loop_ = nullptr;
    return false;
  }

  const char *capture_sink = source_ == 0 ? "true" : "false";
  pw_properties *props = pw_properties_new(nullptr, nullptr);
  if (!props) {
    pw_core_disconnect(core);
    pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
    context_ = nullptr;
    loop_ = nullptr;
    return false;
  }
  pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
  pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Capture");
  pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Communication");
  if (target_node_id_) {
    if (!target_node_name_.empty()) {
      pw_properties_set(props, "target.object", target_node_name_.c_str());
    } else {
      pw_properties_setf(props, "target.object", "%u", *target_node_id_);
    }
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "false");
  } else {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, capture_sink);
  }

  static pw_stream_events stream_events{};
  stream_events.version = PW_VERSION_STREAM_EVENTS;
  stream_events.process = &AudioLinux::on_process;

  stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                 "CoolLiveCaptions Capture",
                                 props,
                                 &stream_events,
                                 this);
  if (!stream_) {
    pw_core_disconnect(core);
    pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
    pw_properties_free(props);
    context_ = nullptr;
    loop_ = nullptr;
    return false;
  }

  pw_core_disconnect(core);

  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = static_cast<uint32_t>(sample_rate_);
  info.channels = kChannels;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;

  uint8_t buffer[256];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const spa_pod *params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

  running_ = true;
  pw_thread_loop_lock(loop_);
  pw_thread_loop_start(loop_);
  int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                              params, 1);
  pw_thread_loop_unlock(loop_);

  if (res < 0) {
    stop();
    return false;
  }

  return true;
}

void AudioLinux::set_target_node(const std::optional<AppInfo> &app) {
  if (app) {
    target_node_id_ = app->id;
    target_node_name_ = app->node_name;
  } else {
    target_node_id_.reset();
    target_node_name_.clear();
  }
}

std::vector<AudioLinux::AppInfo> AudioLinux::list_applications() {
  std::vector<AppInfo> out;
  ensure_init();

  AppListState state;
  state.loop = pw_thread_loop_new("coollivecaptions-app-list", nullptr);
  if (!state.loop) {
    return out;
  }

  state.context = pw_context_new(pw_thread_loop_get_loop(state.loop), nullptr, 0);
  if (!state.context) {
    pw_thread_loop_destroy(state.loop);
    return out;
  }

  state.core = pw_context_connect(state.context, nullptr, 0);
  if (!state.core) {
    pw_context_destroy(state.context);
    pw_thread_loop_destroy(state.loop);
    return out;
  }

  state.registry = pw_core_get_registry(state.core, PW_VERSION_REGISTRY, 0);
  if (!state.registry) {
    pw_core_disconnect(state.core);
    pw_context_destroy(state.context);
    pw_thread_loop_destroy(state.loop);
    return out;
  }

    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = &on_registry_global;
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = &on_core_done;

  pw_thread_loop_start(state.loop);
  pw_thread_loop_lock(state.loop);
  pw_registry_add_listener(state.registry, &state.registry_listener, &registry_events, &state);
  pw_core_add_listener(state.core, &state.core_listener, &core_events, &state);
  state.sync_seq = pw_core_sync(state.core, PW_ID_CORE, 0);
  while (!state.done) {
    pw_thread_loop_wait(state.loop);
  }
  pw_thread_loop_unlock(state.loop);
  pw_thread_loop_stop(state.loop);

  out = std::move(state.apps);
  std::sort(out.begin(), out.end(), [](const AppInfo &a, const AppInfo &b) {
    if (a.label == b.label) {
      return a.id < b.id;
    }
    return a.label < b.label;
  });

  if (state.registry) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(state.registry));
  }
  if (state.core) {
    pw_core_disconnect(state.core);
  }
  if (state.context) {
    pw_context_destroy(state.context);
  }
  if (state.loop) {
    pw_thread_loop_destroy(state.loop);
  }

  return out;
}

void AudioLinux::stop() {
  running_ = false;

  if (loop_) {
    pw_thread_loop_lock(loop_);
    if (stream_) {
      pw_stream_disconnect(stream_);
    }
    pw_thread_loop_unlock(loop_);
    pw_thread_loop_stop(loop_);
  }

  if (stream_) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }

  if (context_) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }

  if (loop_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
}

void AudioLinux::on_process(void *data) {
  auto *self = static_cast<AudioLinux *>(data);
  if (!self || !self->stream_ || !self->handler_) {
    return;
  }

  pw_buffer *buffer = pw_stream_dequeue_buffer(self->stream_);
  if (!buffer) {
    return;
  }
  spa_buffer *b = buffer->buffer;
  if (!b || b->n_datas == 0) {
    pw_stream_queue_buffer(self->stream_, buffer);
    return;
  }

  spa_data *d = b->datas;
  if (!d->data) {
    pw_stream_queue_buffer(self->stream_, buffer);
    return;
  }

  const spa_chunk *c = d->chunk;
  uint32_t offset = c ? c->offset : 0;
  uint32_t size = c ? c->size : d->maxsize;
  uint32_t stride = c && c->stride ? c->stride : static_cast<uint32_t>(sizeof(float) * kChannels);
  if (stride == 0 || size == 0) {
    pw_stream_queue_buffer(self->stream_, buffer);
    return;
  }

  uint8_t *data_ptr = static_cast<uint8_t *>(d->data) + offset;
  uint32_t frames = size / stride;
  if (frames == 0) {
    pw_stream_queue_buffer(self->stream_, buffer);
    return;
  }

  const float *interleaved = reinterpret_cast<const float *>(data_ptr);
  self->mono_.resize(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    float l = interleaved[i * kChannels + 0];
    float r = interleaved[i * kChannels + 1];
    self->mono_[i] = 0.5f * (l + r);
  }

  if (!self->mono_.empty()) {
    self->handler_(self->mono_);
  }

  pw_stream_queue_buffer(self->stream_, buffer);
}

#else

bool AudioLinux::start(size_t sample_rate, int source, SampleHandler handler) {
  (void)sample_rate;
  (void)source;
  (void)handler;
  return false;
}

void AudioLinux::stop() {}

void AudioLinux::set_target_node(const std::optional<AppInfo> &app) {
  (void)app;
}

std::vector<AudioLinux::AppInfo> AudioLinux::list_applications() {
  return {};
}

void AudioLinux::ensure_init() {}
void AudioLinux::on_process(void *) {}

#endif
