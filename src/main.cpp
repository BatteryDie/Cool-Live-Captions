#include <filesystem>
#include <cstdlib>
#include <optional>
#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <map>
#include <future>
#include <chrono>
#include <thread>
#include <atomic>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#include "april_asr.h"
#include "caption.h"
#include "transcription.h"
#include "model.h"
#include "profanity.h"
#include "app_update.h"
#include "lang.h"

#if defined(_WIN32)
#include "audio_win.h"
using AudioBackend = AudioWin;
#elif defined(__APPLE__)
#include "audio_mac.h"
using AudioBackend = AudioMac;
#else
#include "audio_linux.h"
using AudioBackend = AudioLinux;
#endif

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

namespace {

std::string format_timestamp(std::time_t t) {
  std::tm tm_snapshot{};
#if defined(_WIN32)
  localtime_s(&tm_snapshot, &t);
#else
  localtime_r(&t, &tm_snapshot);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_snapshot);
  return std::string(buf);
}

class FileLogger {
 public:
  bool start(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_.open(path, std::ios::out | std::ios::binary);
    if (!file_) {
      return false;
    }
    path_ = path;
    enabled_ = true;
    return true;
  }

  void log_line(const char *level, const std::string &msg) {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    file_ << '[' << format_timestamp(std::time(nullptr)) << "] [" << level << "] " << msg << '\n';
  }

  void shutdown() {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
    file_.close();
    enabled_ = false;
  }

  const std::filesystem::path &path() const { return path_; }

 private:
  std::mutex mutex_;
  std::ofstream file_;
  std::filesystem::path path_;
  bool enabled_ = false;
};

FileLogger g_file_logger;

void log_info(const std::string &msg) {
  std::fprintf(stdout, "[info] %s\n", msg.c_str());
  g_file_logger.log_line("info", msg);
}
constexpr const char *kAppVersion = APP_VERSION_STRING;
constexpr const char *kAppVersionTag = APP_VERSION_TAG;

void log_error(const std::string &msg) {
  std::fprintf(stderr, "[error] %s\n", msg.c_str());
  g_file_logger.log_line("error", msg);
}

std::filesystem::path user_config_dir(const std::filesystem::path &fallback) {
#if defined(_WIN32)
  if (const char *local = std::getenv("LOCALAPPDATA")) {
    return std::filesystem::path(local) / "CoolLiveCaptions";
  }
#endif
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
    return std::filesystem::path(xdg) / "CoolLiveCaptions";
  }
  if (const char *home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".config" / "CoolLiveCaptions";
  }
  return fallback;
}

std::filesystem::path user_home_dir() {
#if defined(_WIN32)
  if (const char *profile = std::getenv("USERPROFILE")) {
    return std::filesystem::path(profile);
  }
  const char *drive = std::getenv("HOMEDRIVE");
  const char *path = std::getenv("HOMEPATH");
  if (drive && path) {
    return std::filesystem::path(std::string(drive) + std::string(path));
  }
#else
  if (const char *home = std::getenv("HOME")) {
    return std::filesystem::path(home);
  }
#endif
  return {};
}

std::filesystem::path documents_root() {
  if (const char *home = std::getenv("USERPROFILE")) {
    return std::filesystem::path(home) / "Documents";
  }
  if (const char *home = std::getenv("HOME")) {
    return std::filesystem::path(home) / "Documents";
  }
  return std::filesystem::current_path();
}

bool enable_file_logging() {
  auto log_dir = documents_root() / "Cool Live Captions" / "Logs";
  const auto now = std::time(nullptr);
  auto log_path = log_dir / (std::string("CoolLiveCaptions-") + std::to_string(static_cast<long long>(now)) + ".log");
  if (!g_file_logger.start(log_path)) {
    return false;
  }
  log_info("File logging enabled: " + log_path.string());
  return true;
}

struct AppSettings {
  bool always_on_top = true;
  bool auto_scroll = true;
  bool break_lines = true;
  bool profanity_filter = false;
  bool lower_case = true;
  bool auto_check_updates = true;
  bool auto_update_models = true;
  int window_width = 1280;
  int window_height = 720;
  float caption_text_scale = 1.0f;
  std::string caption_font_id;
  ImVec4 text_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  ImVec4 background_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
  std::string language = "en";
};

std::string detect_language_from_model(const std::filesystem::path &model_path) {
  auto name = model_path.filename().string();
  auto dot = name.find_last_of('.');
  if (dot == std::string::npos) {
    dot = name.size();
  }
  auto underscore = name.find_last_of('_', dot);
  std::string code;
  if (underscore != std::string::npos && underscore + 1 < dot) {
    code = name.substr(underscore + 1, dot - underscore - 1);
  }
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (code.size() == 2 || code.size() == 3) {
    return code;
  }
  return "en";
}

static std::string detect_system_language() {
#if defined(_WIN32)
  wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {0};
  if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, locale_name, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
      std::string s(needed - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, locale_name, -1, &s[0], needed, nullptr, nullptr);
      // strip modifiers (e.g., en-US -> en)
      auto pos = s.find_first_of("-_@.");
      if (pos != std::string::npos) s = s.substr(0, pos);
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (s.size() >= 2) return s.substr(0, 2);
    }
  }
  return std::string();
#elif defined(__linux__)
  const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", nullptr};
  for (int i = 0; vars[i]; ++i) {
    const char *v = std::getenv(vars[i]);
    if (!v || !v[0]) continue;
    std::string s(v);
    // strip encoding or modifiers (e.g., fr_FR.UTF-8 or en_US@calendar)
    auto pos = s.find_first_of(".@");
    if (pos != std::string::npos) s = s.substr(0, pos);
    // take primary language before underscore
    pos = s.find('_');
    if (pos != std::string::npos) s = s.substr(0, pos);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.size() >= 2) return s.substr(0, 2);
  }
  return std::string();
#else
  return std::string();
#endif
}

std::string apply_lower_case(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool cap_next = true;
  for (char ch : text) {
    unsigned char c = static_cast<unsigned char>(ch);
    bool is_alpha = std::isalpha(c) != 0;
    char lowered = static_cast<char>(std::tolower(c));
    char emit = lowered;
    if (is_alpha && cap_next) {
      emit = static_cast<char>(std::toupper(c));
      cap_next = false;
    }
    out.push_back(emit);
    if (ch == '.' || ch == '!' || ch == '?' || ch == '\n') {
      cap_next = true;
    } else if (is_alpha) {
      cap_next = false;
    }
  }
  return out;
}

std::string format_size(std::uint64_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  int idx = 0;
  while (value >= 1024.0 && idx < 3) {
    value /= 1024.0;
    ++idx;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[idx]);
  return std::string(buf);
}

struct ManagedModelFetchResult {
  bool ok = false;
  std::string error;
  std::vector<ModelManager::RemoteModel> manifest;
};

struct ManagedModelDownloadResult {
  bool ok = false;
  std::string error;
  std::filesystem::path path;
  ModelManager::RemoteModel remote;
};

struct ManagedModelRemoveResult {
  bool ok = false;
  std::string error;
  std::string id;
};

struct ManagedModelUiState {
  bool open_modal = false;
  bool fetch_inflight = false;
  bool download_inflight = false;
  std::string fetch_error;
  std::string download_error;
  std::string import_error;
  bool import_modal_open = false;
  std::filesystem::path import_dir;
  std::string import_selected;
  std::array<char, 512> import_path{};
  std::vector<std::filesystem::path> import_history;
  int import_history_index = -1;
  std::vector<ModelManager::RemoteModel> manifest;
  std::map<std::string, ModelManager::InstalledModel> installed;
  std::optional<std::size_t> selected_online;
  std::optional<std::string> selected_installed;
  std::future<ManagedModelFetchResult> fetch_future;
  std::future<ManagedModelDownloadResult> download_future;
  std::optional<std::filesystem::path> pending_reload;
  bool remove_inflight = false;
  std::future<ManagedModelRemoveResult> remove_future;
  std::optional<std::string> download_target_id;
  std::optional<std::string> remove_target_id;
  std::optional<std::string> pending_remove_id;
  std::optional<std::string> pending_remove_filename;
};

struct AutoUpdateModelsUiState {
  bool updating = false;
  bool download_inflight = false;
  bool close_when_done = false;
  std::vector<ModelManager::RemoteModel> updates;
  std::size_t next_index = 0;
  int completed = 0;
  int failed = 0;
  std::string last_error;
  std::optional<std::filesystem::path> pending_reload;
  std::future<ManagedModelDownloadResult> download_future;
  std::optional<std::string> active_target_id;
};

std::filesystem::path configure_imgui_ini(ImGuiIO &io, const std::filesystem::path &exe_path) {
  static std::string ini_path;
  auto window_dir = user_config_dir(exe_path);
  std::error_code ec;
  std::filesystem::create_directories(window_dir, ec);
  auto target = window_dir / "window.ini";
  ini_path = target.string();
  io.IniFilename = ini_path.c_str();
  return window_dir;
}

std::filesystem::path settings_file(const std::filesystem::path &config_dir) {
  std::error_code ec;
  std::filesystem::create_directories(config_dir, ec);
  return config_dir / "settings.ini";
}

struct FontChoice {
  std::string id;
  std::string label;
  std::string path;
};

std::vector<FontChoice> available_caption_fonts();
const FontChoice *find_font_choice(const std::vector<FontChoice> &fonts, const std::string &id);

std::optional<std::string> first_existing_path(std::initializer_list<const char *> candidates) {
  for (const char *p : candidates) {
    if (p && std::filesystem::exists(p)) {
      return std::string(p);
    }
  }
  return std::nullopt;
}

void load_settings(const std::filesystem::path &path, AppSettings &settings) {
  if (!std::filesystem::exists(path)) {
    return;
  }
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("always_on_top=", 0) == 0) {
      settings.always_on_top = line.find("=1") != std::string::npos;
    } else if (line.rfind("auto_scroll=", 0) == 0) {
      settings.auto_scroll = line.find("=1") != std::string::npos;
    } else if (line.rfind("break_lines=", 0) == 0) {
      settings.break_lines = line.find("=1") != std::string::npos;
    } else if (line.rfind("profanity_filter=", 0) == 0) {
      settings.profanity_filter = line.find("=1") != std::string::npos;
    } else if (line.rfind("lower_case=", 0) == 0) {
      settings.lower_case = line.find("=1") != std::string::npos;
    } else if (line.rfind("auto_check_updates=", 0) == 0) {
      settings.auto_check_updates = line.find("=1") != std::string::npos;
    } else if (line.rfind("auto_update_models=", 0) == 0) {
      settings.auto_update_models = line.find("=1") != std::string::npos;
    } else if (line.rfind("window_width=", 0) == 0) {
      try {
        settings.window_width = std::stoi(line.substr(std::string("window_width=").size()));
      } catch (...) {
      }
    } else if (line.rfind("window_height=", 0) == 0) {
      try {
        settings.window_height = std::stoi(line.substr(std::string("window_height=").size()));
      } catch (...) {
      }
    } else if (line.rfind("caption_text_scale=", 0) == 0) {
      try {
        settings.caption_text_scale = std::stof(line.substr(std::string("caption_text_scale=").size()));
      } catch (...) {
      }
    } else if (line.rfind("caption_font_id=", 0) == 0) {
      settings.caption_font_id = line.substr(std::string("caption_font_id=").size());
    } else if (line.rfind("language=", 0) == 0) {
      settings.language = line.substr(std::string("language=").size());
    } else if (line.rfind("text_color=", 0) == 0) {
      float r = 1.0f;
      float g = 1.0f;
      float b = 1.0f;
      float a = 1.0f;
      if (std::sscanf(line.c_str() + std::string("text_color=").size(), "%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
        settings.text_color = ImVec4(r, g, b, a);
      }
    } else if (line.rfind("background_color=", 0) == 0) {
      float r = 0.0f;
      float g = 0.0f;
      float b = 0.0f;
      float a = 1.0f;
      if (std::sscanf(line.c_str() + std::string("background_color=").size(), "%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
        settings.background_color = ImVec4(r, g, b, a);
      }
    }
  }
  auto fonts = available_caption_fonts();
  if (!find_font_choice(fonts, settings.caption_font_id)) {
    if (!fonts.empty()) {
      settings.caption_font_id = fonts.front().id;
    }
  }
}

void save_settings(const std::filesystem::path &path, const AppSettings &settings) {
  std::vector<std::string> lines;
  if (std::filesystem::exists(path)) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("always_on_top=", 0) == 0 || line.rfind("font_size=", 0) == 0 ||
          line.rfind("auto_scroll=", 0) == 0 || line.rfind("break_lines=", 0) == 0 ||
          line.rfind("profanity_filter=", 0) == 0 || line.rfind("lower_case=", 0) == 0 ||
          line.rfind("auto_check_updates=", 0) == 0 || line.rfind("auto_update_models=", 0) == 0 ||
          line.rfind("window_width=", 0) == 0 || line.rfind("window_height=", 0) == 0 ||
          line.rfind("caption_text_scale=", 0) == 0 ||
          line.rfind("caption_font_id=", 0) == 0 ||
          line.rfind("text_color=", 0) == 0 || line.rfind("background_color=", 0) == 0 ||
          line.rfind("language=", 0) == 0) {
        continue;
      }
      lines.push_back(line);
    }
  }
  lines.push_back(std::string("always_on_top=") + (settings.always_on_top ? "1" : "0"));
  lines.push_back(std::string("auto_scroll=") + (settings.auto_scroll ? "1" : "0"));
  lines.push_back(std::string("break_lines=") + (settings.break_lines ? "1" : "0"));
  lines.push_back(std::string("profanity_filter=") + (settings.profanity_filter ? "1" : "0"));
  lines.push_back(std::string("lower_case=") + (settings.lower_case ? "1" : "0"));
  lines.push_back(std::string("auto_check_updates=") + (settings.auto_check_updates ? "1" : "0"));
  lines.push_back(std::string("auto_update_models=") + (settings.auto_update_models ? "1" : "0"));
  lines.push_back(std::string("window_width=") + std::to_string(settings.window_width));
  lines.push_back(std::string("window_height=") + std::to_string(settings.window_height));
  lines.push_back(std::string("caption_text_scale=") + std::to_string(settings.caption_text_scale));
  lines.push_back(std::string("caption_font_id=") + settings.caption_font_id);
  lines.push_back(std::string("language=") + settings.language);
  {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "text_color=%.3f,%.3f,%.3f,%.3f",
                  settings.text_color.x, settings.text_color.y, settings.text_color.z, settings.text_color.w);
    lines.push_back(buf);
  }
  {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "background_color=%.3f,%.3f,%.3f,%.3f",
                  settings.background_color.x, settings.background_color.y, settings.background_color.z, settings.background_color.w);
    lines.push_back(buf);
  }
  std::ofstream out(path, std::ios::trunc);
  for (const auto &l : lines) {
    out << l << '\n';
  }
}

void configure_style() {
  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 2.0f;
}

const char *pick_default_ui_font_path() {
#if defined(_WIN32)
  const char *paths[] = {
    "C:/Windows/Fonts/segoeui.ttf"
  };
#elif defined(__APPLE__)
  const char *paths[] = {
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/SFNSText.ttf",
    "/System/Library/Fonts/SF-Pro-Text-Regular.otf"
  };
#else
  const char *paths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
  };
#endif
  for (const char *path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

std::vector<FontChoice> available_caption_fonts() {
  std::vector<FontChoice> fonts;
#if defined(_WIN32)
  if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) {
    fonts.push_back({"segoeui", "Segoe UI", "C:/Windows/Fonts/segoeui.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/arial.ttf")) {
    fonts.push_back({"arial", "Arial", "C:/Windows/Fonts/arial.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/verdana.ttf")) {
    fonts.push_back({"verdana", "Verdana", "C:/Windows/Fonts/verdana.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/tahoma.ttf")) {
    fonts.push_back({"tahoma", "Tahoma", "C:/Windows/Fonts/tahoma.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/trebuc.ttf")) {
    fonts.push_back({"trebuchet-ms", "Trebuchet MS", "C:/Windows/Fonts/trebuc.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/impact.ttf")) {
    fonts.push_back({"impact", "Impact", "C:/Windows/Fonts/impact.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/times.ttf")) {
    fonts.push_back({"times-new-roman", "Times New Roman", "C:/Windows/Fonts/times.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/georgia.ttf")) {
    fonts.push_back({"georgia", "Georgia", "C:/Windows/Fonts/georgia.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/pala.ttf")) {
    fonts.push_back({"palatino", "Palatino", "C:/Windows/Fonts/pala.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/garamond.ttf")) {
    fonts.push_back({"garamond", "Garamond", "C:/Windows/Fonts/garamond.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/consola.ttf")) {
    fonts.push_back({"consolas", "Consolas", "C:/Windows/Fonts/consola.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/cour.ttf")) {
    fonts.push_back({"courier-new", "Courier New", "C:/Windows/Fonts/cour.ttf"});
  }
  if (std::filesystem::exists("C:/Windows/Fonts/lucon.ttf")) {
    fonts.push_back({"lucida-console", "Lucida Console", "C:/Windows/Fonts/lucon.ttf"});
  }
#elif defined(__APPLE__)
  const char *mac_fonts[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
    "/System/Library/Fonts/Supplemental/Tahoma.ttf",
    "/System/Library/Fonts/Supplemental/Trebuchet MS.ttf",
    "/System/Library/Fonts/Supplemental/Impact.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
    "/System/Library/Fonts/Supplemental/Georgia.ttf",
    "/System/Library/Fonts/Supplemental/Palatino.ttf",
    "/System/Library/Fonts/Supplemental/Garamond.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Supplemental/Lucida Console.ttf"
  };
  const char *mac_ids[] = {
    "arial",
    "verdana",
    "tahoma",
    "trebuchet-ms",
    "impact",
    "times-new-roman",
    "georgia",
    "palatino",
    "garamond",
    "courier-new",
    "lucida-console"
  };
  const char *mac_labels[] = {
    "Arial",
    "Verdana",
    "Tahoma",
    "Trebuchet MS",
    "Impact",
    "Times New Roman",
    "Georgia",
    "Palatino",
    "Garamond",
    "Courier New",
    "Lucida Console"
  };
  for (size_t i = 0; i < sizeof(mac_fonts) / sizeof(mac_fonts[0]); ++i) {
    if (std::filesystem::exists(mac_fonts[i])) {
      fonts.push_back({mac_ids[i], mac_labels[i], mac_fonts[i]});
    }
  }
#else
  // Linux distros frequently ship DejaVu/Liberation/Noto by default, and
  // ms core fonts (if installed) are often lowercase on disk (e.g. arial.ttf).
  struct LinuxFontSpec {
    const char *id;
    const char *label;
    std::initializer_list<const char *> candidates;
  };
  const LinuxFontSpec linux_specs[] = {
    {"dejavu-sans", "DejaVu Sans", {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf"
    }},
    {"dejavu-serif", "DejaVu Serif", {
      "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
      "/usr/share/fonts/dejavu/DejaVuSerif.ttf"
    }},
    {"liberation-sans", "Liberation Sans", {
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
      "/usr/share/fonts/liberation2/LiberationSans-Regular.ttf"
    }},
    {"noto-sans", "Noto Sans", {
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf"
    }},
    // Microsoft core fonts (if installed)
    {"arial", "Arial", {
      "/usr/share/fonts/truetype/msttcorefonts/Arial.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/arial.ttf"
    }},
    {"verdana", "Verdana", {
      "/usr/share/fonts/truetype/msttcorefonts/Verdana.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/verdana.ttf"
    }},
    {"tahoma", "Tahoma", {
      "/usr/share/fonts/truetype/msttcorefonts/Tahoma.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/tahoma.ttf"
    }},
    {"trebuchet-ms", "Trebuchet MS", {
      "/usr/share/fonts/truetype/msttcorefonts/Trebuchet_MS.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/trebuc.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/trebuchet_ms.ttf"
    }},
    {"impact", "Impact", {
      "/usr/share/fonts/truetype/msttcorefonts/Impact.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/impact.ttf"
    }},
    {"times-new-roman", "Times New Roman", {
      "/usr/share/fonts/truetype/msttcorefonts/Times_New_Roman.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/times.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/times_new_roman.ttf"
    }},
    {"georgia", "Georgia", {
      "/usr/share/fonts/truetype/msttcorefonts/Georgia.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/georgia.ttf"
    }},
    {"palatino", "Palatino", {
      "/usr/share/fonts/truetype/msttcorefonts/Palatino.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/pala.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/palatino.ttf"
    }},
    {"garamond", "Garamond", {
      "/usr/share/fonts/truetype/msttcorefonts/Garamond.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/garamond.ttf"
    }},
    {"courier-new", "Courier New", {
      "/usr/share/fonts/truetype/msttcorefonts/Courier_New.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/cour.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/courier_new.ttf"
    }},
    {"lucida-console", "Lucida Console", {
      "/usr/share/fonts/truetype/msttcorefonts/Lucida_Console.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/lucon.ttf",
      "/usr/share/fonts/truetype/msttcorefonts/lucida_console.ttf"
    }}
  };
  for (const auto &spec : linux_specs) {
    if (auto p = first_existing_path(spec.candidates)) {
      fonts.push_back({spec.id, spec.label, *p});
    }
  }
#endif
  return fonts;
}

const FontChoice *find_font_choice(const std::vector<FontChoice> &fonts, const std::string &id) {
  for (const auto &font : fonts) {
    if (id == font.id) {
      return &font;
    }
  }
  return nullptr;
}

ImFont *configure_fonts(const std::filesystem::path &base, float ui_size, float caption_size, const char *caption_path) {
  ImGuiIO &io = ImGui::GetIO();
  (void)base;
  auto add_font = [&](const char *path, float size) {
    ImFontConfig cfg;
    cfg.SizePixels = size;
    return io.Fonts->AddFontFromFileTTF(path, size, &cfg);
  };

  const char *ui_path = pick_default_ui_font_path();
  ImFont *ui_font = nullptr;
  if (ui_path) {
    ui_font = add_font(ui_path, ui_size);
  }
  if (!ui_font) {
    ImFontConfig ui_cfg;
    ui_cfg.SizePixels = ui_size;
    ui_font = io.Fonts->AddFontDefault(&ui_cfg);
  }
  if (ui_font) {
    io.FontDefault = ui_font;
  }

  ImFont *caption_font = nullptr;
  if (caption_path && std::filesystem::exists(caption_path)) {
    caption_font = add_font(caption_path, caption_size);
  }
  if (!caption_font && ui_path) {
    caption_font = add_font(ui_path, caption_size);
  }
  if (!caption_font) {
    ImFontConfig caption_cfg;
    caption_cfg.SizePixels = caption_size;
    caption_font = io.Fonts->AddFontDefault(&caption_cfg);
  }
  return caption_font ? caption_font : ui_font;
}

bool open_folder(const std::filesystem::path &path) {
#if defined(_WIN32)
  std::wstring wpath = path.wstring();
  auto hinst = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(hinst) > 32;
#elif defined(__APPLE__)
  std::string cmd = "open \"" + path.string() + "\"";
  return std::system(cmd.c_str()) == 0;
#else
  std::string cmd = "xdg-open \"" + path.string() + "\"";
  return std::system(cmd.c_str()) == 0;
#endif
}
} // namespace
enum class AudioSourceKind { Desktop, Microphone, Application };

int run_app(int argc, char **argv) {
  (void)argc;

  bool use_dev_manifest = false;
  bool use_gpu = false;
  bool enable_logging = false;
  for (int i = 1; i < argc; ++i) {
    std::string a(argv[i]);
    if (a == "--dev-manifest") {
      use_dev_manifest = true;
    } else if (a == "--gpu") {
      use_gpu = true;
    } else if (a == "--logging") {
      enable_logging = true;
    }
  }

  if (enable_logging && !enable_file_logging()) {
    log_error("Failed to enable file logging.");
  }

  if (!glfwInit()) {
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
#if defined(__linux__) && !defined(__APPLE__)
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "coollivecaptions");
  glfwWindowHintString(GLFW_X11_CLASS_NAME, "CoolLiveCaptions");
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "CoolLiveCaptions");
#endif

  std::string window_title = std::string("Cool Live Captions ") + kAppVersionTag;
  GLFWwindow *window = glfwCreateWindow(1280, 720, window_title.c_str(), nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  bool refresh_models = false;

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(__APPLE__)
  ImGui_ImplOpenGL3_Init("#version 150");
#else
  ImGui_ImplOpenGL3_Init("#version 130");
#endif

  std::filesystem::path exe_path;
#if defined(_WIN32)
  {
    wchar_t module_path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) != 0) {
      int needed = WideCharToMultiByte(CP_UTF8, 0, module_path, -1, nullptr, 0, nullptr, nullptr);
      if (needed > 0) {
        std::string exe_utf8(needed - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, module_path, -1, &exe_utf8[0], needed, nullptr, nullptr);
        exe_path = std::filesystem::path(exe_utf8).parent_path();
      }
    }
    if (exe_path.empty()) {
      exe_path = std::filesystem::absolute(argv[0]).parent_path();
    }
  }
#else
  try {
    // Resolve the actual executable path from /proc/self/exe (works on Linux)
    std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe");
    exe_path = resolved.parent_path();
  } catch (...) {
    exe_path = std::filesystem::absolute(argv[0]).parent_path();
  }
#endif
  auto window_dir = configure_imgui_ini(io, exe_path);
  auto settings_path = settings_file(window_dir);
  bool settings_exists = std::filesystem::exists(settings_path);
  AppSettings settings{};
  if (settings_exists) {
    load_settings(settings_path, settings);
  } else {
    // first run: try to detect system language from the OS
    std::string syslang = detect_system_language();
    if (!syslang.empty()) {
      settings.language = syslang;
    }
  }
  LocaleManager locale;
  locale.load(exe_path, settings.language);
  std::string tr_audio_sources, tr_desktop_audio, tr_microphone, tr_select_application;
  std::string tr_caption_models, tr_model_manager, tr_settings, tr_help;
  std::string tr_yes, tr_no, tr_caption_model_required_title, tr_caption_model_required_text;
  std::string tr_check_updates_now, tr_always_on_top, tr_full_screen, tr_appearance;
  std::string tr_auto_scroll, tr_break_lines, tr_lower_case_text, tr_profanity_filter;
  std::string tr_documentation, tr_release_notes, tr_about;
  std::string tr_close, tr_update_check_wait, tr_update_failed_fmt, tr_update_yes_open_link, tr_update_cancel;
  std::string tr_update_up_to_date_fmt, tr_ok, tr_about_developer_fmt, tr_about_copyright;

  auto update_translations = [&]() {
    tr_audio_sources = locale.t("menu.audio_sources");
    tr_desktop_audio = locale.t("menu.desktop_audio");
    tr_microphone = locale.t("menu.microphone");
    tr_select_application = locale.t("menu.select_application");
    tr_caption_models = locale.t("menu.caption_models");
    tr_model_manager = locale.t("menu.model_manager");
    tr_settings = locale.t("menu.settings");
    tr_help = locale.t("menu.help");
    tr_yes = locale.t("dialog.yes");
    tr_no = locale.t("dialog.no");
    tr_caption_model_required_title = locale.t("modal.caption_model_required_title");
    tr_caption_model_required_text = locale.t("modal.caption_model_required_text");
    tr_check_updates_now = locale.t("menu.check_updates_now");
    tr_always_on_top = locale.t("menu.always_on_top");
    tr_full_screen = locale.t("menu.full_screen");
    tr_appearance = locale.t("menu.appearance");
    tr_auto_scroll = locale.t("menu.auto_scroll");
    tr_break_lines = locale.t("menu.break_lines");
    tr_lower_case_text = locale.t("menu.lower_case_text");
    tr_profanity_filter = locale.t("menu.profanity_filter");
    tr_documentation = locale.t("menu.documentation");
    tr_release_notes = locale.t("menu.release_notes");
    tr_about = locale.t("menu.about");
    tr_close = locale.t("dialog.close");
    tr_update_check_wait = locale.t("update.checking");
    tr_update_failed_fmt = locale.t("update.failed_fmt");
    tr_update_yes_open_link = locale.t("update.yes_open_link");
    tr_update_cancel = locale.t("update.cancel");
    tr_update_up_to_date_fmt = locale.t("update.up_to_date_fmt");
    tr_ok = locale.t("dialog.ok");
    tr_about_developer_fmt = locale.t("about.developer_fmt");
    tr_about_copyright = locale.t("about.copyright");
  };
  update_translations();
  if (settings.window_width > 0 && settings.window_height > 0) {
    glfwSetWindowSize(window, settings.window_width, settings.window_height);
  }
  ModelManager model_manager(exe_path, use_dev_manifest);
  model_manager.refresh();
  std::string initial_caption_path;
  const char *initial_caption_path_cstr = nullptr;
  {
    auto fonts = available_caption_fonts();
    if (fonts.empty()) {
      settings.caption_font_id.clear();
    } else if (!find_font_choice(fonts, settings.caption_font_id)) {
      settings.caption_font_id = fonts.front().id;
    }
    if (const auto *choice = find_font_choice(fonts, settings.caption_font_id)) {
      initial_caption_path = choice->path;
      initial_caption_path_cstr = initial_caption_path.c_str();
    }
  }
  ImFont *caption_font = configure_fonts(exe_path, 26.0f, 26.0f * settings.caption_text_scale, initial_caption_path_cstr);
  configure_style();
  glfwSetWindowAttrib(window, GLFW_FLOATING, settings.always_on_top ? GLFW_TRUE : GLFW_FALSE);

  ManagedModelUiState managed_ui;
  AutoUpdateModelsUiState auto_update_ui;
  std::thread model_update_thread;
  std::atomic<bool> model_updates_available{false};
  std::vector<ModelManager::RemoteModel> model_updates_list;
  std::mutex model_updates_mutex;
  bool startup_model_update_pending = settings.auto_update_models;
  bool startup_model_update_started = false;

  // Determine profanity directory from several candidates (exe-relative, resources, or system paths)
  std::filesystem::path profanity_dir;
  std::vector<std::filesystem::path> profanity_candidates = {
    exe_path / "profanity",
    exe_path / "resources" / "profanity",
    exe_path.parent_path() / "resources" / "profanity",
    std::filesystem::path("/usr/share/coollivecaptions/resources/profanity"),
    std::filesystem::path("/opt/coollivecaptions/resources/profanity")
  };
  for (const auto &p : profanity_candidates) {
    if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) { profanity_dir = p; break; }
  }

  CaptionView caption;
  TranscriptionWriter writer;
  AprilAsrEngine engine;
  AudioBackend audio;
  AudioSourceKind audio_source = AudioSourceKind::Desktop;
#if (defined(__linux__) && !defined(__APPLE__)) || defined(_WIN32)
  std::vector<AudioBackend::AppInfo> app_list;
  std::optional<AudioBackend::AppInfo> selected_app;
#endif
  ProfanityFilter profanity;
  app_update::UpdateState update_state;
  bool auto_update_result_handled = false;
  if (settings.auto_check_updates) {
    log_info("Automatic update check at startup");
    app_update::start_update_check(update_state, false);
  }

  auto start_model_update_check = [&]() {
    if (model_update_thread.joinable()) {
      model_update_thread.join();
    }
    // Check manifest and if updates exist, notify user (do not download automatically)
    model_update_thread = std::thread([&model_manager, &model_updates_available, &model_updates_list, &model_updates_mutex]() {
      std::vector<ModelManager::RemoteModel> manifest;
      std::string error;
      if (!model_manager.fetch_manifest(manifest, error)) {
        log_error("Model auto-update manifest failed: " + error);
        return;
      }
      auto installed = model_manager.installed_models();
      std::vector<ModelManager::RemoteModel> updates;
      for (const auto &remote : manifest) {
        auto it = installed.find(remote.id);
        if (it == installed.end()) {
          continue;
        }
        if (!it->second.managed) {
          continue;
        }
        if (it->second.version == remote.version) {
          continue;
        }
        updates.push_back(remote);
      }
      if (!updates.empty()) {
        std::lock_guard<std::mutex> lock(model_updates_mutex);
        model_updates_list = std::move(updates);
        model_updates_available = true;
      }
    });
  };

  auto models = model_manager.models();
  std::optional<std::filesystem::path> active_model;
  bool engine_ready = false;
  if (!models.empty()) {
    active_model = models.front();
    caption.set_active_model(active_model->filename().string());
    engine_ready = engine.load_model(*active_model) && engine.start();
    if (engine_ready) {
      log_info("Loaded model: " + active_model->filename().string());
    } else {
      log_error("Failed to load model: " + active_model->filename().string());
    }
    if (!profanity.load(profanity_dir, detect_language_from_model(active_model->filename()))) {
      log_error("Profanity list not found for model language: " + active_model->filename().string());
    }
  }
  if (!active_model) {
    log_error("No caption models found.");
  }

  auto start_audio = [&]() {
    if (!engine_ready) {
      return;
    }
#if defined(_WIN32)
    AudioBackend::Source src = AudioBackend::Source::Loopback;
    std::string source_label = "Desktop";
    if (audio_source == AudioSourceKind::Microphone) {
      src = AudioBackend::Source::Microphone;
      source_label = "Microphone";
    } else if (audio_source == AudioSourceKind::Application) {
#if defined(_WIN32)
      if (!selected_app) {
        log_error("No application selected for audio capture.");
        return;
      }
      audio.set_target_node(selected_app);
      src = AudioBackend::Source::Application;
      source_label = "Application";
      if (selected_app && !selected_app->label.empty()) {
        source_label += " (" + selected_app->label + ")";
      }
#endif
    } else {
      audio.set_target_node(std::nullopt);
    }
#else
    int src = audio_source == AudioSourceKind::Desktop ? 0 : 1;
    std::string source_label;
    if (audio_source == AudioSourceKind::Desktop) {
      source_label = "Desktop";
    } else if (audio_source == AudioSourceKind::Microphone) {
      source_label = "Microphone";
    } else {
      source_label = "Application";
    }
#if defined(__linux__) && !defined(__APPLE__)
    if (audio_source == AudioSourceKind::Application) {
      if (!selected_app) {
        log_error("No application selected for audio capture.");
        return;
      }
      audio.set_target_node(selected_app);
      if (!selected_app->label.empty()) {
        source_label += " (" + selected_app->label + ")";
      }
    } else {
      audio.set_target_node(std::nullopt);
    }
#endif
#endif
    log_info(std::string("Starting audio: ") + source_label +
             ", model rate " + std::to_string(engine.sample_rate()));
    audio.start(engine.sample_rate(), src, [&](const std::vector<float> &samples) { engine.push_audio(samples); });
  };

  if (engine_ready) {
    start_audio();
  }

  bool auto_scroll_enabled = settings.auto_scroll;
  bool profanity_filter_enabled = settings.profanity_filter;
  bool lower_case_enabled = settings.lower_case;
  bool first_run_modal = models.empty();
  bool appearance_open = false;
  bool appearance_was_open = false;
  bool about_open = false;
  bool fullscreen_enabled = false;
  bool fullscreen_key_down = false;
  bool spacebar_key_down = false;
  bool f9_key_down = false;
  int windowed_x = 0;
  int windowed_y = 0;
  int windowed_w = settings.window_width > 0 ? settings.window_width : 1280;
  int windowed_h = settings.window_height > 0 ? settings.window_height : 720;
  const float appearance_panel_width = 300.0f;
  ImVec4 pending_text_color = settings.text_color;
  ImVec4 pending_background_color = settings.background_color;
  float pending_caption_text_scale = settings.caption_text_scale;
  std::string pending_caption_font_id = settings.caption_font_id;
  bool rebuild_caption_font = false;

  auto rebuild_fonts = [&]() {
    ImGuiIO &font_io = ImGui::GetIO();
    font_io.Fonts->Clear();
    std::string caption_path_storage;
    const char *caption_path = nullptr;
    auto fonts = available_caption_fonts();
    if (const auto *choice = find_font_choice(fonts, settings.caption_font_id)) {
      caption_path_storage = choice->path;
      caption_path = caption_path_storage.c_str();
    }
    caption_font = configure_fonts(exe_path, 26.0f, 26.0f * settings.caption_text_scale, caption_path);
    if (!caption_font) {
      caption_font = ImGui::GetFont();
    }
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
  };

  auto start_manifest_fetch = [&]() {
    if (managed_ui.fetch_inflight) {
      return;
    }
    managed_ui.fetch_error.clear();
    managed_ui.download_error.clear();
    managed_ui.import_error.clear();
    managed_ui.pending_reload.reset();
    managed_ui.manifest.clear();
    managed_ui.selected_online.reset();
    managed_ui.fetch_inflight = true;
    managed_ui.fetch_future = std::async(std::launch::async, [&model_manager]() {
      ManagedModelFetchResult result;
      result.ok = model_manager.fetch_manifest(result.manifest, result.error);
      return result;
    });
  };

  auto start_download = [&](const ModelManager::RemoteModel &remote) {
    if (managed_ui.download_inflight) {
      return;
    }
    managed_ui.download_error.clear();
    // If the selected remote corresponds to an installed model that's currently active,
    // unload it first and mark for reload after download completes to avoid freezes.
    auto it = managed_ui.installed.find(remote.id);
    if (it != managed_ui.installed.end() && active_model) {
      if (active_model->filename().string() == it->second.filename) {
        log_info(std::string("Unloading active model before reinstall: id=") + remote.id + " filename=" + it->second.filename);
        managed_ui.pending_reload = model_manager.user_dir() / it->second.filename;
        caption.clear();
        caption.set_active_model(std::string());
        audio.stop();
        engine.stop();
        engine_ready = false;
        active_model.reset();
        log_info(std::string("Unloaded active model for reinstall: id=") + remote.id + " filename=" + it->second.filename);
      }
    }
    managed_ui.download_target_id = remote.id;
    managed_ui.download_inflight = true;
    managed_ui.download_future = std::async(std::launch::async, [&model_manager, remote]() {
      ManagedModelDownloadResult result;
      result.remote = remote;
      result.ok = model_manager.download_model(remote, result.error, &result.path);
      return result;
    });
  };

  auto start_auto_update_download = [&](const ModelManager::RemoteModel &remote) {
    if (auto_update_ui.download_inflight) {
      return;
    }
    auto_update_ui.last_error.clear();
    // If the target corresponds to an installed model that's currently active,
    // unload it first and mark for reload after download completes to avoid freezes.
    auto installed = model_manager.installed_models();
    auto it = installed.find(remote.id);
    if (it != installed.end() && active_model) {
      if (active_model->filename().string() == it->second.filename) {
        log_info(std::string("Unloading active model before auto-update: id=") + remote.id + " filename=" + it->second.filename);
        auto_update_ui.pending_reload = model_manager.user_dir() / it->second.filename;
        caption.clear();
        caption.set_active_model(std::string());
        audio.stop();
        engine.stop();
        engine_ready = false;
        active_model.reset();
      }
    }
    auto_update_ui.active_target_id = remote.id;
    auto_update_ui.download_inflight = true;
    auto_update_ui.download_future = std::async(std::launch::async, [&model_manager, remote]() {
      ManagedModelDownloadResult result;
      result.remote = remote;
      result.ok = model_manager.download_model(remote, result.error, &result.path);
      return result;
    });
  };

  auto toggle_fullscreen = [&]() {
    fullscreen_enabled = !fullscreen_enabled;
    if (fullscreen_enabled) {
      glfwGetWindowPos(window, &windowed_x, &windowed_y);
      glfwGetWindowSize(window, &windowed_w, &windowed_h);
      GLFWmonitor *monitor = glfwGetPrimaryMonitor();
      const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
      if (monitor && mode) {
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
      }
    } else {
      glfwSetWindowMonitor(window, nullptr, windowed_x, windowed_y, windowed_w, windowed_h, 0);
    }
  };

  while (!glfwWindowShouldClose(window)) {
    app_update::finalize_update_thread(update_state);
    // Avoid a busy-loop at idle (some systems/drivers may not block reliably on swap).
    // Still wakes immediately on input, and caps idle redraw rate.
    glfwWaitEventsTimeout(0.008); // ~120Hz max when idle

    bool f11_down = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
    if (f11_down && !fullscreen_key_down) {
      toggle_fullscreen();
    }
    fullscreen_key_down = f11_down;

    bool spacebar_down = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (spacebar_down && !spacebar_key_down) {
      auto_scroll_enabled = !auto_scroll_enabled;
      settings.auto_scroll = auto_scroll_enabled;
      save_settings(settings_path, settings);
    }
    spacebar_key_down = spacebar_down;

    bool f9_down = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
    if (f9_down && !f9_key_down) {
      settings.always_on_top = !settings.always_on_top;
      glfwSetWindowAttrib(window, GLFW_FLOATING, settings.always_on_top ? GLFW_TRUE : GLFW_FALSE);
      save_settings(settings_path, settings);
    }
    f9_key_down = f9_down;

    // Startup sequencing: run in-app update check first, then check for managed model updates.
    if (startup_model_update_pending && !startup_model_update_started) {
      if (!settings.auto_check_updates) {
        start_model_update_check();
        startup_model_update_started = true;
        startup_model_update_pending = false;
      } else {
        bool checking = false;
        bool has_result = false;
        {
          std::lock_guard<std::mutex> lock(update_state.mutex);
          checking = update_state.checking;
          has_result = update_state.has_result;
        }
        if (!checking && has_result) {
          start_model_update_check();
          startup_model_update_started = true;
          startup_model_update_pending = false;
        }
      }
    }

    if (settings.auto_check_updates && !auto_update_result_handled) {
      bool checking = false;
      bool has_result = false;
      app_update::UpdateResult snapshot;
      {
        std::lock_guard<std::mutex> lock(update_state.mutex);
        checking = update_state.checking;
        has_result = update_state.has_result;
        snapshot = update_state.result;
      }
      if (!checking && has_result) {
        auto_update_result_handled = true;
        if (snapshot.success && app_update::compare_versions(kAppVersionTag, snapshot.latest_tag) < 0) {
          std::lock_guard<std::mutex> lock(update_state.mutex);
          update_state.show_modal = true;
        }
      }
    }

    if (refresh_models) {
      refresh_models = false;
      model_manager.refresh();
      auto updated = model_manager.models();
      if (!updated.empty()) {
        if (!active_model || std::find(updated.begin(), updated.end(), *active_model) == updated.end()) {
          active_model = updated.front();
          caption.clear();
          caption.set_active_model(active_model->filename().string());
          audio.stop();
          engine.stop();
          engine_ready = engine.load_model(*active_model) && engine.start();
          if (engine_ready) {
            log_info("Loaded model: " + active_model->filename().string());
            if (!profanity.load(profanity_dir, detect_language_from_model(active_model->filename()))) {
              log_error("Profanity list not found for model language: " + active_model->filename().string());
            }
            start_audio();
          } else {
            log_error("Failed to load model: " + active_model->filename().string());
          }
        }
      } else {
        active_model.reset();
        caption.clear();
        engine.stop();
        engine_ready = false;
        audio.stop();
        log_error("No caption models found.");
      }
      models = std::move(updated);
    }

    if (auto text = engine.poll_text()) {
      auto normalized = lower_case_enabled ? apply_lower_case(*text) : *text;
      auto filtered = profanity_filter_enabled ? profanity.filter(normalized) : normalized;
      if (settings.break_lines && !caption.buffer().empty()) {
        caption.append("\n");
      }
      caption.append(filtered);
      writer.write_line(filtered);
    }
    auto partial_raw = engine.peek_partial();
    std::optional<std::string> partial_filtered;
    if (partial_raw && !partial_raw->empty()) {
      auto normalized_partial = lower_case_enabled ? apply_lower_case(*partial_raw) : *partial_raw;
      partial_filtered = profanity_filter_enabled ? profanity.filter(normalized_partial) : normalized_partial;
    }

    if (managed_ui.fetch_inflight && managed_ui.fetch_future.valid() &&
        managed_ui.fetch_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto result = managed_ui.fetch_future.get();
      managed_ui.fetch_inflight = false;
      managed_ui.fetch_error = result.ok ? std::string() : result.error;
      if (result.ok) {
        managed_ui.manifest = std::move(result.manifest);
        managed_ui.installed = model_manager.installed_models();
        if (!managed_ui.manifest.empty()) {
          managed_ui.selected_online = 0;
        }
      }
    }

    if (managed_ui.download_inflight && managed_ui.download_future.valid() &&
        managed_ui.download_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto result = managed_ui.download_future.get();
      managed_ui.download_inflight = false;
      managed_ui.download_target_id.reset();
      managed_ui.download_error = result.ok ? std::string() : result.error;
      if (result.ok) {
        model_manager.record_install(result.remote, result.path);
        managed_ui.installed = model_manager.installed_models();
        if (managed_ui.pending_reload && result.path == *managed_ui.pending_reload) {
          active_model = result.path;
          caption.clear();
          caption.set_active_model(active_model->filename().string());
          audio.stop();
          engine.stop();
          engine_ready = engine.load_model(*active_model) && engine.start();
          if (engine_ready) {
            if (!profanity.load(profanity_dir, detect_language_from_model(active_model->filename()))) {
              log_error("Profanity list not found for model language: " + active_model->filename().string());
            }
            start_audio();
            (void)0;
          } else {
            log_error("Failed to reload reinstalled model: " + active_model->filename().string());
          }
          managed_ui.pending_reload.reset();
        }
        refresh_models = true;
      }
    }

    if (auto_update_ui.download_inflight && auto_update_ui.download_future.valid() &&
        auto_update_ui.download_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto result = auto_update_ui.download_future.get();
      auto_update_ui.download_inflight = false;
      auto_update_ui.active_target_id.reset();
      if (result.ok) {
        model_manager.record_install(result.remote, result.path);
        managed_ui.installed = model_manager.installed_models();
        if (auto_update_ui.pending_reload && result.path == *auto_update_ui.pending_reload) {
          active_model = result.path;
          caption.clear();
          caption.set_active_model(active_model->filename().string());
          audio.stop();
          engine.stop();
          engine_ready = engine.load_model(*active_model) && engine.start();
          if (engine_ready) {
            if (!profanity.load(profanity_dir, detect_language_from_model(active_model->filename()))) {
              log_error("Profanity list not found for model language: " + active_model->filename().string());
            }
            start_audio();
          } else {
            log_error("Failed to reload updated model: " + active_model->filename().string());
          }
          auto_update_ui.pending_reload.reset();
        }
        refresh_models = true;
        auto_update_ui.completed += 1;
      } else {
        auto_update_ui.failed += 1;
        auto_update_ui.last_error = result.error;
      }

      if (auto_update_ui.updating) {
        if (auto_update_ui.next_index < auto_update_ui.updates.size()) {
          start_auto_update_download(auto_update_ui.updates[auto_update_ui.next_index++]);
        } else {
          auto_update_ui.updating = false;
          auto_update_ui.close_when_done = true;
        }
      }
    }

    if (managed_ui.remove_inflight && managed_ui.pending_remove_id && !managed_ui.remove_future.valid()) {
      if (managed_ui.pending_remove_filename && active_model) {
        if (active_model->filename().string() == *managed_ui.pending_remove_filename) {
          (void)0;
          caption.clear();
          caption.set_active_model(std::string());
          audio.stop();
          engine.stop();
          engine_ready = false;
          active_model.reset();
          (void)0;
        }
      }
      std::string id_copy = *managed_ui.pending_remove_id;
      (void)0;
      managed_ui.remove_future = std::async(std::launch::async, [&model_manager, id_copy]() {
        ManagedModelRemoveResult r;
        r.id = id_copy;
        r.ok = model_manager.remove_installed(id_copy, r.error);
        return r;
      });
      managed_ui.pending_remove_id.reset();
      managed_ui.pending_remove_filename.reset();
    }

    if (managed_ui.remove_inflight && managed_ui.remove_future.valid() &&
        managed_ui.remove_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto r = managed_ui.remove_future.get();
      managed_ui.remove_inflight = false;
      managed_ui.remove_target_id.reset();
      if (r.ok) {
        managed_ui.installed = model_manager.installed_models();
        managed_ui.download_error.clear();
        refresh_models = true;
      } else {
        managed_ui.download_error = r.error;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (first_run_modal) {
      ImGui::OpenPopup(tr_caption_model_required_title.c_str());
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(640.0f, FLT_MAX));
    if (ImGui::BeginPopupModal(tr_caption_model_required_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s", tr_caption_model_required_text.c_str());
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      const float button_w = 110.0f;
      const float buttons_w = button_w * 2.0f + ImGui::GetStyle().ItemSpacing.x;
      const float center_x = (ImGui::GetContentRegionAvail().x - buttons_w) * 0.5f;
      ImGui::SetCursorPosX(std::max(0.0f, center_x));
      ImGui::BeginGroup();
      if (ImGui::Button(tr_yes.c_str(), ImVec2(button_w, 0))) {
        model_manager.sync_installed_with_disk();
        managed_ui.open_modal = true;
        managed_ui.installed = model_manager.installed_models();
        managed_ui.selected_installed.reset();
        managed_ui.selected_online.reset();
        managed_ui.import_error.clear();
        start_manifest_fetch();
        refresh_models = true;
        first_run_modal = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(tr_no.c_str(), ImVec2(button_w, 0))) {
        first_run_modal = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndGroup();
      ImGui::EndPopup();
    }

    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu(tr_audio_sources.c_str())) {
        if (ImGui::MenuItem(tr_desktop_audio.c_str(), nullptr, audio_source == AudioSourceKind::Desktop)) {
          audio_source = AudioSourceKind::Desktop;
#if (defined(__linux__) && !defined(__APPLE__)) || defined(_WIN32)
          selected_app.reset();
#endif
          audio.stop();
          start_audio();
        }
        if (ImGui::MenuItem(tr_microphone.c_str(), nullptr, audio_source == AudioSourceKind::Microphone)) {
          audio_source = AudioSourceKind::Microphone;
#if (defined(__linux__) && !defined(__APPLE__)) || defined(_WIN32)
          selected_app.reset();
#endif
          audio.stop();
          start_audio();
        }
#if (defined(__linux__) && !defined(__APPLE__)) || defined(_WIN32)
        std::string select_app_label = tr_select_application;
        if (selected_app && !selected_app->label.empty()) {
          select_app_label += " (" + selected_app->label + ")";
        }
        if (ImGui::BeginMenu(select_app_label.c_str())) {
          static double app_list_last_refresh = 0.0;
          const double now = ImGui::GetTime();
          if (ImGui::IsWindowAppearing() && (now - app_list_last_refresh) >= 2.0) {
            app_list = audio.list_applications();
            app_list_last_refresh = now;
          }
          if (app_list.empty()) {
            ImGui::TextDisabled("No applications playing audio");
          }
          for (const auto &app : app_list) {
            bool selected = audio_source == AudioSourceKind::Application && selected_app && selected_app->id == app.id;
            if (ImGui::MenuItem(app.label.c_str(), nullptr, selected)) {
              selected_app = app;
              audio_source = AudioSourceKind::Application;
              audio.stop();
              start_audio();
            }
          }
          ImGui::EndMenu();
        }
#endif
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(tr_caption_models.c_str())) {
      if (ImGui::MenuItem(tr_model_manager.c_str())) {
          model_manager.sync_installed_with_disk();
          managed_ui.open_modal = true;
          managed_ui.installed = model_manager.installed_models();
          managed_ui.selected_installed.reset();
          managed_ui.selected_online.reset();
          managed_ui.import_error.clear();
          start_manifest_fetch();
          refresh_models = true;
        }
        ImGui::Separator();
        if (models.empty()) {
          ImGui::TextDisabled("Caption Model(s) not found");
        }
        for (std::size_t i = 0; i < models.size(); ++i) {
          bool selected = active_model && *active_model == models[i];
          std::string label = models[i].stem().string();
          std::string lang = detect_language_from_model(models[i].filename());
          label += " [" + lang + "]";
          if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
            active_model = models[i];
            caption.clear();
            caption.set_active_model(models[i].filename().string());
            audio.stop();
            engine.stop();
            engine_ready = engine.load_model(*active_model) && engine.start();
            if (engine_ready) {
              log_info("Loaded model: " + active_model->filename().string());
              if (!profanity.load(profanity_dir, detect_language_from_model(active_model->filename()))) {
                log_error("Profanity list not found for model language: " + active_model->filename().string());
              }
              start_audio();
            } else {
              log_error("Failed to load model: " + active_model->filename().string());
            }
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(tr_settings.c_str())) {
        ImGui::TextDisabled(locale.t("menu.section_system").c_str());
        if (ImGui::BeginMenu(locale.t("menu.localization").c_str())) {
          auto langs = locale.available_languages();
          for (const auto &p : langs) {
            bool selected = p.first == settings.language;
            if (ImGui::MenuItem(p.second.c_str(), nullptr, selected)) {
              settings.language = p.first;
              save_settings(settings_path, settings);
              locale.load(exe_path, settings.language);
              update_translations();
            }
          }
          ImGui::EndMenu();
        }
        bool auto_update_menu = settings.auto_check_updates;
        if (ImGui::MenuItem(locale.t("menu.auto_check_updates").c_str(), nullptr, auto_update_menu)) {
          settings.auto_check_updates = !auto_update_menu;
          save_settings(settings_path, settings);
          if (settings.auto_check_updates) {
            app_update::start_update_check(update_state, false);
          }
        }
        if (ImGui::MenuItem(tr_check_updates_now.c_str())) {
          app_update::start_update_check(update_state, true);
        }
        ImGui::Separator();

        ImGui::TextDisabled(locale.t("menu.section_windows").c_str());
        bool atop = settings.always_on_top;
        if (ImGui::MenuItem(tr_always_on_top.c_str(), "F9", atop)) {
          settings.always_on_top = !atop;
          glfwSetWindowAttrib(window, GLFW_FLOATING, settings.always_on_top ? GLFW_TRUE : GLFW_FALSE);
          save_settings(settings_path, settings);
        }
        if (ImGui::MenuItem(tr_full_screen.c_str(), "F11", fullscreen_enabled)) {
          toggle_fullscreen();
        }
        ImGui::Separator();

        ImGui::TextDisabled(locale.t("menu.section_captions").c_str());
        if (ImGui::MenuItem(tr_appearance.c_str())) {
          appearance_open = true;
        }
        bool auto_scroll_menu = auto_scroll_enabled;
        if (ImGui::MenuItem(tr_auto_scroll.c_str(), "Space", auto_scroll_menu)) {
          auto_scroll_enabled = !auto_scroll_menu;
          settings.auto_scroll = auto_scroll_enabled;
          save_settings(settings_path, settings);
        }
        bool break_lines_menu = settings.break_lines;
        if (ImGui::MenuItem(tr_break_lines.c_str(), nullptr, break_lines_menu)) {
          settings.break_lines = !break_lines_menu;
          save_settings(settings_path, settings);
        }
        bool lower_case_menu = lower_case_enabled;
        if (ImGui::MenuItem(tr_lower_case_text.c_str(), nullptr, lower_case_menu)) {
          lower_case_enabled = !lower_case_menu;
          settings.lower_case = lower_case_enabled;
          save_settings(settings_path, settings);
        }
        ImGui::Separator();

        ImGui::TextDisabled(locale.t("menu.section_extras").c_str());
        bool profanity_menu = profanity_filter_enabled;
        if (ImGui::MenuItem(tr_profanity_filter.c_str(), nullptr, profanity_menu)) {
          profanity_filter_enabled = !profanity_menu;
          settings.profanity_filter = profanity_filter_enabled;
          save_settings(settings_path, settings);
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(tr_help.c_str())) {
        if (ImGui::MenuItem(tr_documentation.c_str())) {
          app_update::open_url("https://github.com/BatteryDie/Cool-Live-Captions/wiki");
        }
        if (ImGui::MenuItem(tr_release_notes.c_str())) {
          std::string url = std::string("https://github.com/BatteryDie/Cool-Live-Captions/releases/tag/") + kAppVersionTag;
          app_update::open_url(url);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr_about.c_str())) {
          about_open = true;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }

    if (about_open) {
      ImGui::OpenPopup(tr_about.c_str());
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(520.0f, FLT_MAX));
    bool about_keep_open = true;
    if (ImGui::BeginPopupModal(tr_about.c_str(), &about_keep_open, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        about_open = false;
      }

      ImGui::TextWrapped(locale.t("about.tagline").c_str());
      ImGui::Separator();
      ImGui::Text((locale.t("about.version_fmt")).c_str(), kAppVersion);
      ImGui::Text((tr_about_developer_fmt).c_str(), locale.t("about.developer_name").c_str());
      ImGui::TextUnformatted(tr_about_copyright.c_str());
      ImGui::Separator();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextWrapped("Cool Live Captions is GPL-3.0 licensed and includes open source software under other licenses.");
      ImGui::TextWrapped("You can download and build the source code for this release from GitHub:");
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.6f, 0.95f, 1.0f));
      ImGui::TextWrapped("BatteryDie/Cool-Live-Captions");
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImVec2 link_min = ImGui::GetItemRectMin();
        ImVec2 link_max = ImGui::GetItemRectMax();
        ImU32 link_col = ImGui::GetColorU32(ImGuiCol_Text);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(link_min.x, link_max.y + 1.0f), ImVec2(link_max.x, link_max.y + 1.0f), link_col, 1.0f);
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        app_update::open_url("https://github.com/BatteryDie/Cool-Live-Captions");
      }
      ImGui::PopTextWrapPos();
      ImGui::Separator();
      ImGui::TextUnformatted("Third Party Licenses:");
      auto link_row = [&](const char *label, const char *url) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.6f, 0.95f, 1.0f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          ImVec2 link_min = ImGui::GetItemRectMin();
          ImVec2 link_max = ImGui::GetItemRectMax();
          ImU32 link_col = ImGui::GetColorU32(ImGuiCol_Text);
          ImGui::GetWindowDrawList()->AddLine(ImVec2(link_min.x, link_max.y + 1.0f), ImVec2(link_max.x, link_max.y + 1.0f), link_col, 1.0f);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          app_update::open_url(url);
        }
      };
      link_row("ONNX Runtime (MIT License)", "https://github.com/microsoft/onnxruntime");
      ImGui::TextUnformatted("Copyright © Microsoft Corporation");
      ImGui::Spacing();
      link_row("Dear ImGui (MIT License)", "https://github.com/ocornut/imgui");
      ImGui::TextUnformatted("Copyright © 2014-2024 Omar Cornut");
      ImGui::Spacing();
      link_row("GLFW (zlib/libpng License)", "https://github.com/glfw/glfw");
      ImGui::TextUnformatted("Copyright © 2002-2006 Marcus Geelnard");
      ImGui::TextUnformatted("Copyright © 2006-2019 Camilla Lowy");
      ImGui::Spacing();
      link_row("april-asr (GPL-3.0 License)", "https://github.com/abb128/april-asr");
      ImGui::TextUnformatted("Copyright © 2024 abb128 and contributors");
      ImGui::EndPopup();
    }
    if (about_open && !about_keep_open) {
      about_open = false;
    }

    if (appearance_open) {
      if (!appearance_was_open) {
        pending_text_color = settings.text_color;
        pending_background_color = settings.background_color;
        pending_caption_text_scale = settings.caption_text_scale;
        pending_caption_font_id = settings.caption_font_id;
        appearance_was_open = true;
      }
      float menu_height = ImGui::GetFrameHeight();
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - appearance_panel_width, menu_height), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(appearance_panel_width, io.DisplaySize.y - menu_height), ImGuiCond_Always);
      ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
      if (ImGui::Begin(tr_appearance.c_str(), &appearance_open, panel_flags)) {
        ImGui::TextUnformatted(locale.t("appearance.caption_colors").c_str());
        ImGui::ColorEdit4(locale.t("appearance.text").c_str(), &pending_text_color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4(locale.t("appearance.background").c_str(), &pending_background_color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        auto fonts = available_caption_fonts();
        if (!find_font_choice(fonts, pending_caption_font_id)) {
          pending_caption_font_id = fonts.empty() ? std::string() : std::string(fonts.front().id);
        }
        std::string current_label_storage = "Default";
        const char *current_label = current_label_storage.c_str();
        if (const auto *choice = find_font_choice(fonts, pending_caption_font_id)) {
          current_label_storage = choice->label;
          current_label = current_label_storage.c_str();
        }
        ImGui::TextUnformatted(locale.t("appearance.font").c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##Font", current_label)) {
          for (const auto &font : fonts) {
            bool selected = pending_caption_font_id == font.id;
            if (ImGui::Selectable(font.label.c_str(), selected)) {
              pending_caption_font_id = font.id;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted(locale.t("appearance.text_size").c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##TextSize", &pending_caption_text_scale, 0.6f, 2.0f, "%.2fx");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(locale.t("appearance.apply").c_str(), ImVec2(-FLT_MIN, 0))) {
          settings.text_color = pending_text_color;
          settings.background_color = pending_background_color;
          settings.caption_text_scale = pending_caption_text_scale;
          settings.caption_font_id = pending_caption_font_id;
          rebuild_caption_font = true;
          save_settings(settings_path, settings);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(locale.t("appearance.reset_defaults").c_str(), ImVec2(-FLT_MIN, 0))) {
          pending_text_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
          pending_background_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
          pending_caption_text_scale = 1.0f;
          pending_caption_font_id = fonts.empty() ? std::string() : std::string(fonts.front().id);
          settings.text_color = pending_text_color;
          settings.background_color = pending_background_color;
          settings.caption_text_scale = pending_caption_text_scale;
          settings.caption_font_id = pending_caption_font_id;
          rebuild_caption_font = true;
          save_settings(settings_path, settings);
        }
      }
      ImGui::End();
    } else if (appearance_was_open) {
      appearance_was_open = false;
    }

    if (managed_ui.open_modal) {
      ImGui::OpenPopup(locale.t("modal.caption_model_manager_title").c_str());
      managed_ui.open_modal = false;
    }

    bool model_modal_open = true;
    if (ImGui::BeginPopupModal(locale.t("modal.caption_model_manager_title").c_str(), &model_modal_open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImVec2 modal_size = ImVec2(io.DisplaySize.x * 0.8f, io.DisplaySize.y * 0.8f);
      ImGui::SetWindowSize(modal_size);
      ImGui::SetWindowPos(ImVec2(io.DisplaySize.x * 0.1f, io.DisplaySize.y * 0.1f));

      if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !managed_ui.download_inflight) {
        ImGui::CloseCurrentPopup();
      }

      float content_height = ImGui::GetContentRegionAvail().y;
      float list_width = ImGui::GetContentRegionAvail().x * 0.55f;
      float info_width = ImGui::GetContentRegionAvail().x - list_width - 8.0f;

      ImGui::BeginChild("models_list", ImVec2(list_width, content_height), true);
      float split_gap = ImGui::GetStyle().ItemSpacing.y;
      float installed_height = (content_height - split_gap) * 0.5f;

      ImGui::BeginChild("models_list_installed", ImVec2(0, installed_height), true);
      ImGui::TextUnformatted(locale.t("model.installed_models").c_str());
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), locale.t("model.import_local").c_str());
      {
        if (ImGui::IsItemHovered()) {
          ImDrawList *draw_list = ImGui::GetWindowDrawList();
          ImVec2 min = ImGui::GetItemRectMin();
          ImVec2 max = ImGui::GetItemRectMax();
          draw_list->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), IM_COL32(51, 153, 255, 255), 1.0f);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          managed_ui.import_error.clear();
          managed_ui.import_selected.clear();
          if (managed_ui.import_dir.empty()) {
            managed_ui.import_dir = user_home_dir();
            if (managed_ui.import_dir.empty()) {
              managed_ui.import_dir = model_manager.user_dir();
            }
            if (managed_ui.import_dir.empty()) {
              managed_ui.import_dir = std::filesystem::current_path();
            }
          }
          std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
          managed_ui.import_modal_open = true;
        }
      }
      ImGui::Separator();
      if (managed_ui.installed.empty()) {
        ImGui::TextDisabled(locale.t("model.no_installed").c_str());
      } else {
        for (const auto &kv : managed_ui.installed) {
          const auto &model = kv.second;
          bool selected = managed_ui.selected_installed && *managed_ui.selected_installed == kv.first;
          std::filesystem::path model_path(model.filename);
          std::string display_name = model_path.stem().string();
          std::string language = detect_language_from_model(model_path);
          std::string label = display_name + " [" + language + "]";
          if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, 0))) {
            managed_ui.selected_installed = kv.first;
            managed_ui.selected_online.reset();
            managed_ui.download_error.clear();
            managed_ui.import_error.clear();
          }
          if (!model.version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", model.version.c_str());
          }
        }
      }
      ImGui::EndChild();

      ImGui::Spacing();

      ImGui::BeginChild("models_list_online", ImVec2(0, 0), true);
      ImGui::TextUnformatted(locale.t("model.online_models").c_str());
      bool auto_update_models_checkbox = settings.auto_update_models;
      if (ImGui::Checkbox(locale.t("model.auto_update_managed").c_str(), &auto_update_models_checkbox)) {
        settings.auto_update_models = auto_update_models_checkbox;
        save_settings(settings_path, settings);
        if (settings.auto_update_models) {
          start_model_update_check();
        }
      }
      ImGui::Separator();
      if (managed_ui.fetch_inflight) {
        ImGui::TextUnformatted(locale.t("model.fetching_manifest").c_str());
      } else if (!managed_ui.fetch_error.empty()) {
        ImGui::TextDisabled(locale.t("model.manifest_unavailable").c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(locale.t("model.failed_fetch_fmt").c_str(), managed_ui.fetch_error.c_str());
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), locale.t("model.retry").c_str());
        if (ImGui::IsItemHovered()) {
          ImDrawList *draw_list = ImGui::GetWindowDrawList();
          ImVec2 min = ImGui::GetItemRectMin();
          ImVec2 max = ImGui::GetItemRectMax();
          draw_list->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), IM_COL32(51, 153, 255, 255), 1.0f);
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            start_manifest_fetch();
          }
        }
      } else if (managed_ui.manifest.empty()) {
        ImGui::TextDisabled(locale.t("model.no_online").c_str());
      } else {
        for (std::size_t i = 0; i < managed_ui.manifest.size(); ++i) {
          const auto &remote = managed_ui.manifest[i];
          bool selected = managed_ui.selected_online && *managed_ui.selected_online == i;
          std::filesystem::path remote_path(remote.filename);
          std::string display_name = remote_path.stem().string();
          std::string label = display_name + " [" + remote.language + "]";
          if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, 0))) {
            managed_ui.selected_online = i;
            managed_ui.selected_installed.reset();
            managed_ui.download_error.clear();
            managed_ui.import_error.clear();
          }
          if (!remote.version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", remote.version.c_str());
          }
        }
      }
      ImGui::EndChild();
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("model_info", ImVec2(info_width, content_height), true);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y * 0.6f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 0.6f));

      std::map<std::string, ModelManager::InstalledModel> installed_snapshot = managed_ui.installed;
      bool has_selected_online = managed_ui.selected_online && *managed_ui.selected_online < managed_ui.manifest.size();
      bool has_selected_installed = managed_ui.selected_installed &&
                                    installed_snapshot.find(*managed_ui.selected_installed) != installed_snapshot.end();

      ModelManager::RemoteModel selected_remote;
      std::map<std::string, ModelManager::InstalledModel>::const_iterator inst_it = installed_snapshot.end();
      if (has_selected_online) {
        selected_remote = managed_ui.manifest[*managed_ui.selected_online];
        inst_it = installed_snapshot.find(selected_remote.id);
      } else if (has_selected_installed) {
        inst_it = installed_snapshot.find(*managed_ui.selected_installed);
      }

      if (!managed_ui.import_error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", managed_ui.import_error.c_str());
        ImGui::Spacing();
      }
      if (!managed_ui.download_error.empty()) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", managed_ui.download_error.c_str());
        ImGui::Spacing();
      }

        if (has_selected_online) {
        bool is_installed = inst_it != installed_snapshot.end();
        bool is_managed = is_installed && inst_it->second.managed;
        bool needs_update = is_managed && (inst_it->second.version != selected_remote.version);
        bool disable_primary = managed_ui.download_inflight || !managed_ui.fetch_error.empty();
        ImGui::BeginDisabled(disable_primary);
        std::string primary_label_str;
        if (managed_ui.download_inflight) {
          primary_label_str = locale.t("model.downloading");
        } else if (is_installed && is_managed) {
          primary_label_str = needs_update ? locale.t("model.update") : locale.t("model.reinstall");
        } else {
          primary_label_str = locale.t("model.install");
        }
        if (ImGui::Button(primary_label_str.c_str(), ImVec2(-FLT_MIN, 0))) {
          start_download(selected_remote);
        }
        ImGui::EndDisabled();
      } else if (has_selected_installed) {
        bool removing_this = managed_ui.remove_inflight && managed_ui.remove_target_id &&
                             *managed_ui.remove_target_id == *managed_ui.selected_installed;
        bool remove_disabled = managed_ui.remove_inflight;
        std::string remove_label_str = removing_this ? locale.t("model.removing") : locale.t("model.remove");
        ImGui::BeginDisabled(remove_disabled);
        if (ImGui::Button(remove_label_str.c_str(), ImVec2(-FLT_MIN, 0))) {
          managed_ui.remove_inflight = true;
          managed_ui.remove_target_id = *managed_ui.selected_installed;
          managed_ui.pending_remove_id = *managed_ui.selected_installed;
          std::string installed_filename;
          auto it_inst = managed_ui.installed.find(*managed_ui.selected_installed);
          if (it_inst != managed_ui.installed.end()) installed_filename = it_inst->second.filename;
          managed_ui.pending_remove_filename = installed_filename;
        }
        ImGui::EndDisabled();
      } else {
        ImGui::TextDisabled(locale.t("model.select_model_details").c_str());
      }

      ImGui::Separator();
      ImGui::Spacing();

      float scrollable_h = std::max(0.0f, ImGui::GetContentRegionAvail().y);
      ImGui::BeginChild("model_info_scrollable", ImVec2(0, scrollable_h), false);

      if (has_selected_online) {
        const auto &remote = managed_ui.manifest[*managed_ui.selected_online];
        auto it = managed_ui.installed.find(remote.id);

        ImGui::Spacing();
        if (!remote.name.empty()) {
          ImGui::TextUnformatted(remote.name.c_str());
        }
        if (!remote.author.empty()) {
          ImGui::Text((locale.t("model.author_fmt")).c_str(), remote.author.c_str());
        }
        ImGui::Text((locale.t("model.language_fmt")).c_str(), remote.language.c_str());
        ImGui::Text((locale.t("model.version_fmt")).c_str(), remote.version.c_str());
        ImGui::Text((locale.t("model.size_fmt")).c_str(), format_size(remote.size_bytes).c_str());

        if (!remote.url_website.empty()) {
          std::string domain = remote.url_website;
          auto pos = domain.find("://");
          if (pos != std::string::npos) domain = domain.substr(pos + 3);
          auto slash = domain.find('/');
          if (slash != std::string::npos) domain = domain.substr(0, slash);
          if (domain.rfind("www.", 0) == 0) domain = domain.substr(4);

          ImGui::TextUnformatted(locale.t("model.link").c_str());
          ImGui::SameLine();
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.6f, 0.95f, 1.0f));
          ImGui::TextUnformatted(domain.c_str());
          ImGui::PopStyleColor();

          ImVec2 item_min = ImGui::GetItemRectMin();
          ImVec2 item_max = ImGui::GetItemRectMax();
          if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(item_min.x, item_max.y + 1.0f), ImVec2(item_max.x, item_max.y + 1.0f), col, 1.0f);
            if (ImGui::IsMouseClicked(0)) {
              app_update::open_url(remote.url_website);
            }
          }
        }

        if (it != managed_ui.installed.end()) {
          ImGui::Text((locale.t("model.installed_version_fmt")).c_str(), it->second.version.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        

        if (!remote.description.empty()) {
          ImGui::TextWrapped("%s", remote.description.c_str());
          ImGui::Spacing();
        }

        (void)0;
      } else if (has_selected_installed) {
        const auto &installed = installed_snapshot.find(*managed_ui.selected_installed)->second;
        ImGui::Spacing();
        ImGui::Text((locale.t("model.filename_fmt")).c_str(), installed.filename.c_str());
        ImGui::Text((locale.t("model.version_fmt")).c_str(), installed.version.c_str());
        ImGui::Text((locale.t("model.source_fmt")).c_str(), installed.managed ? locale.t("model.source_online").c_str() : locale.t("model.source_local").c_str());

        if (installed.managed) {
          bool shown = false;
          for (const auto &remote : managed_ui.manifest) {
            if (remote.id == *managed_ui.selected_installed) {
              ImGui::Spacing();
              ImGui::Separator();
              ImGui::Spacing();
              if (!remote.name.empty()) {
                ImGui::TextUnformatted(remote.name.c_str());
              }
              if (!remote.author.empty()) {
                ImGui::Text((locale.t("model.author_fmt")).c_str(), remote.author.c_str());
              }
              ImGui::Text((locale.t("model.language_fmt")).c_str(), remote.language.c_str());
              shown = true;
              break;
            }
          }
          if (!shown && installed.metadata.has_value()) {
            const auto &meta = installed.metadata.value();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (!meta.name.empty()) {
              ImGui::TextUnformatted(meta.name.c_str());
            }
            if (!meta.author.empty()) {
              ImGui::Text("Author: %s", meta.author.c_str());
            }
            if (!meta.language.empty()) {
              ImGui::Text("Language: %s", meta.language.c_str());
            }
          }
        }
      } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a model to view details");

        (void)0;
      }

      ImGui::EndChild();
      ImGui::PopStyleVar(2);

      ImGui::EndChild();

      ImGui::EndPopup();
    }

    if (managed_ui.import_modal_open) {
      ImGui::OpenPopup(locale.t("modal.import_caption_model_title").c_str());
      managed_ui.import_modal_open = false;
    }

    float import_menu_height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0.0f, import_menu_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - import_menu_height), ImGuiCond_Always);
    if (ImGui::BeginPopupModal(locale.t("modal.import_caption_model_title").c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::TextUnformatted(locale.t("import.select_model_file").c_str());
      if (!managed_ui.import_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", managed_ui.import_error.c_str());
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // Helper to add directory to navigation history
      auto add_to_history = [&](const std::filesystem::path &path) {
        if (managed_ui.import_history_index >= 0 && 
            managed_ui.import_history_index < (int)managed_ui.import_history.size() &&
            managed_ui.import_history[managed_ui.import_history_index] == path) {
          return; // Already at this location
        }
        // Remove any forward history when navigating to a new location
        if (managed_ui.import_history_index < (int)managed_ui.import_history.size() - 1) {
          managed_ui.import_history.erase(
            managed_ui.import_history.begin() + managed_ui.import_history_index + 1,
            managed_ui.import_history.end()
          );
        }
        managed_ui.import_history.push_back(path);
        managed_ui.import_history_index = (int)managed_ui.import_history.size() - 1;
      };

      // Navigation buttons
      std::string back_label_str = locale.t("import.back");
      std::string forward_label_str = locale.t("import.forward");
      std::string go_label_str = locale.t("import.go");
      std::string up_label_str = locale.t("import.up");
      const char *back_label = back_label_str.c_str();
      const char *forward_label = forward_label_str.c_str();
      const char *go_label = go_label_str.c_str();
      const char *up_label = up_label_str.c_str();
      
      ImVec2 back_text = ImGui::CalcTextSize(back_label);
      ImVec2 forward_text = ImGui::CalcTextSize(forward_label);
      ImVec2 go_text = ImGui::CalcTextSize(go_label);
      ImVec2 up_text = ImGui::CalcTextSize(up_label);
      float pad_x = ImGui::GetStyle().FramePadding.x;
      float back_w = back_text.x + pad_x * 2.0f;
      float forward_w = forward_text.x + pad_x * 2.0f;
      float go_w = go_text.x + pad_x * 2.0f;
      float up_w = up_text.x + pad_x * 2.0f;
      float spacing = ImGui::GetStyle().ItemSpacing.x;
      float avail = ImGui::GetContentRegionAvail().x;
      float input_w = avail - (back_w + forward_w + go_w + up_w + spacing * 4.0f);
      if (input_w < 80.0f) input_w = std::max(40.0f, avail - (back_w + forward_w + go_w + up_w + spacing * 3.0f));

      // Back button
      bool can_go_back = managed_ui.import_history_index > 0;
      ImGui::BeginDisabled(!can_go_back);
      if (ImGui::Button(back_label, ImVec2(back_w, 0))) {
        if (can_go_back) {
          managed_ui.import_history_index--;
          managed_ui.import_dir = managed_ui.import_history[managed_ui.import_history_index];
          std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
          managed_ui.import_selected.clear();
          managed_ui.import_error.clear();
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();

      // Forward button
      bool can_go_forward = managed_ui.import_history_index >= 0 && 
                            managed_ui.import_history_index < (int)managed_ui.import_history.size() - 1;
      ImGui::BeginDisabled(!can_go_forward);
      if (ImGui::Button(forward_label, ImVec2(forward_w, 0))) {
        if (can_go_forward) {
          managed_ui.import_history_index++;
          managed_ui.import_dir = managed_ui.import_history[managed_ui.import_history_index];
          std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
          managed_ui.import_selected.clear();
          managed_ui.import_error.clear();
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();

      // Up button
      if (ImGui::Button(up_label, ImVec2(up_w, 0))) {
        if (!managed_ui.import_dir.empty()) {
          managed_ui.import_dir = managed_ui.import_dir.parent_path();
          add_to_history(managed_ui.import_dir);
          std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
          managed_ui.import_selected.clear();
        }
      }
      ImGui::SameLine();

      // Path input
      ImGui::PushItemWidth(input_w);
      if (ImGui::InputText("##import_path", managed_ui.import_path.data(), managed_ui.import_path.size())) {
        // keep buffer in sync; actual navigation happens via Go
      }
      ImGui::PopItemWidth();
      ImGui::SameLine();

      // Go button
      if (ImGui::Button(go_label, ImVec2(go_w, 0))) {
        std::filesystem::path input_path = managed_ui.import_path.data();
        std::error_code ec;
        if (std::filesystem::is_directory(input_path, ec)) {
          managed_ui.import_dir = input_path;
          add_to_history(managed_ui.import_dir);
          managed_ui.import_selected.clear();
          managed_ui.import_error.clear();
        } else if (std::filesystem::is_regular_file(input_path, ec)) {
          managed_ui.import_dir = input_path.parent_path();
          add_to_history(managed_ui.import_dir);
          managed_ui.import_selected = input_path.filename().string();
          managed_ui.import_error.clear();
        } else {
          managed_ui.import_error = locale.t("import.path_not_found");
        }
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      auto is_model_file = [](const std::filesystem::path &p) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".april" || ext == ".onnx" || ext == ".ort";
      };

      auto do_import = [&]() {
        if (managed_ui.import_selected.empty()) {
          return;
        }
        std::filesystem::path source = managed_ui.import_dir / managed_ui.import_selected;
        std::string error;
        std::filesystem::path imported;
        if (model_manager.import_model(source, error, &imported)) {
          managed_ui.import_error.clear();
          managed_ui.installed = model_manager.installed_models();
          managed_ui.selected_installed = std::string("local:") + imported.filename().string();
          managed_ui.selected_online.reset();
          refresh_models = true;
          ImGui::CloseCurrentPopup();
          managed_ui.open_modal = true;
        } else {
          managed_ui.import_error = error;
        }
      };

      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        managed_ui.open_modal = true;
      }

      const float import_footer_h = ImGui::GetFrameHeightWithSpacing() * 2.2f;
      ImGui::BeginGroup();
      {
        // Calculate sidebar width dynamically based on content
        std::vector<std::string> sidebar_labels = {
          locale.t("import.sidebar_home"),
          locale.t("import.sidebar_documents"),
          locale.t("import.sidebar_downloads"),
          locale.t("import.sidebar_desktop"),
          locale.t("import.sidebar_root"),
        };
        float max_label_width = 0.0f;
        for (const auto &label : sidebar_labels) {
          ImVec2 text_size = ImGui::CalcTextSize(label.c_str());
          max_label_width = std::max(max_label_width, text_size.x);
        }
        float sidebar_width = max_label_width + ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
        sidebar_width = std::max(sidebar_width, 120.0f); // minimum width
        sidebar_width = std::min(sidebar_width, 220.0f); // maximum width
        
        ImGui::BeginChild("import_sidebar", ImVec2(sidebar_width, -import_footer_h), true);
        
        // Location group
        ImGui::TextDisabled("Location:");
        std::filesystem::path home_dir = user_home_dir();
        std::vector<std::pair<std::string, std::filesystem::path>> std_locations = {
          {locale.t("import.sidebar_home"), home_dir},
          {locale.t("import.sidebar_desktop"), home_dir / "Desktop"},
          {locale.t("import.sidebar_documents"), home_dir / "Documents"},
          {locale.t("import.sidebar_downloads"), home_dir / "Downloads"},
        };
        
        for (const auto &[label, path] : std_locations) {
          std::error_code ec;
          if (std::filesystem::exists(path, ec)) {
            bool is_current = path == managed_ui.import_dir;
            if (ImGui::Selectable(label.c_str(), is_current)) {
              managed_ui.import_dir = path;
              add_to_history(managed_ui.import_dir);
              std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
              managed_ui.import_selected.clear();
              managed_ui.import_error.clear();
            }
          }
        }
        
        ImGui::Spacing();
        ImGui::TextDisabled("Drive:");
        
        // Drive group
        std::vector<std::pair<std::string, std::filesystem::path>> drives;
#if defined(_WIN32)
        DWORD drive_mask = GetLogicalDrives();
        for (char letter = 'A'; letter <= 'Z'; ++letter) {
          DWORD bit = 1u << (letter - 'A');
          if ((drive_mask & bit) == 0) {
            continue;
          }
          std::string root = std::string(1, letter) + ":\\";
          std::error_code ec;
          if (std::filesystem::exists(root, ec)) {
            drives.emplace_back(std::string(1, letter) + ":", std::filesystem::path(root));
          }
        }
#else
        drives.emplace_back(locale.t("import.sidebar_root"), std::filesystem::path("/"));
        {
          std::error_code ec;
          if (std::filesystem::exists("/media", ec)) {
            drives.emplace_back("/media", std::filesystem::path("/media"));
          }
        }
        {
          std::error_code ec;
          if (std::filesystem::exists("/mnt", ec)) {
            drives.emplace_back("/mnt", std::filesystem::path("/mnt"));
          }
        }
#endif
        
        for (const auto &[label, path] : drives) {
          std::error_code ec;
          if (std::filesystem::exists(path, ec)) {
            bool is_current = path == managed_ui.import_dir;
            if (ImGui::Selectable(label.c_str(), is_current)) {
              managed_ui.import_dir = path;
              add_to_history(managed_ui.import_dir);
              std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
              managed_ui.import_selected.clear();
              managed_ui.import_error.clear();
            }
          }
        }
        
        ImGui::EndChild();
      }
      
      ImGui::SameLine();
      
      {
        ImGui::BeginChild("import_list", ImVec2(0, -import_footer_h), true);
        if (managed_ui.import_dir.empty()) {
          ImGui::TextDisabled(locale.t("import.no_directory_selected").c_str());
        } else {
          std::vector<std::filesystem::directory_entry> dirs;
          std::vector<std::filesystem::directory_entry> files;
          std::error_code ec;
          for (const auto &entry : std::filesystem::directory_iterator(managed_ui.import_dir, ec)) {
            if (entry.is_directory(ec)) {
              dirs.push_back(entry);
            } else if (entry.is_regular_file(ec) && is_model_file(entry.path())) {
              files.push_back(entry);
            }
          }
          std::sort(dirs.begin(), dirs.end(), [](const auto &a, const auto &b) { return a.path().filename() < b.path().filename(); });
          std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.path().filename() < b.path().filename(); });

          for (const auto &entry : dirs) {
            std::string label = entry.path().filename().string() + "/";
            if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, 0))) {
              managed_ui.import_dir = entry.path();
              add_to_history(managed_ui.import_dir);
              std::snprintf(managed_ui.import_path.data(), managed_ui.import_path.size(), "%s", managed_ui.import_dir.string().c_str());
              managed_ui.import_selected.clear();
            }
          }
          for (const auto &entry : files) {
            std::string label = entry.path().filename().string();
            bool selected = managed_ui.import_selected == label;
            if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, 0))) {
              managed_ui.import_selected = label;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
              managed_ui.import_selected = label;
              do_import();
            }
          }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Right-align the action buttons
        float button_width = 120.0f;
        float buttons_total_width = button_width * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        float avail_width = ImGui::GetContentRegionAvail().x;
        float offset = avail_width - buttons_total_width;
        if (offset > 0) {
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }

        bool can_import = !managed_ui.import_selected.empty();
        ImGui::BeginDisabled(!can_import);
        if (ImGui::Button(locale.t("import.import_btn").c_str(), ImVec2(button_width, 0))) {
          do_import();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(locale.t("import.cancel").c_str(), ImVec2(button_width, 0))) {
          ImGui::CloseCurrentPopup();
          managed_ui.open_modal = true;
        }
      }
      ImGui::EndGroup();

      ImGui::EndPopup();
    }

    bool open_update_popup = false;
    {
      std::lock_guard<std::mutex> lock(update_state.mutex);
      if (update_state.show_modal) {
        open_update_popup = true;
        update_state.show_modal = false;
      }
    }
    if (open_update_popup) {
      ImGui::OpenPopup(locale.t("update.title").c_str());
    }

    bool update_popup_visible = false;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(440.0f, 0.0f), ImVec2(640.0f, FLT_MAX));
    if (ImGui::BeginPopupModal(locale.t("update.title").c_str(), nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      update_popup_visible = true;
      
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
      }
      
      bool checking = false;
      bool has_result = false;
      app_update::UpdateResult snapshot;
      {
        std::lock_guard<std::mutex> lock(update_state.mutex);
        checking = update_state.checking;
        has_result = update_state.has_result;
        snapshot = update_state.result;
      }

      if (checking || !has_result) {
        ImGui::TextUnformatted(tr_update_check_wait.c_str());
        ImGuiStyle &style = ImGui::GetStyle();
        float close_width = ImGui::CalcTextSize(tr_close.c_str()).x + style.FramePadding.x * 2.0f;
        float close_avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (close_avail - close_width) * 0.5f);
        if (ImGui::Button(tr_close.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
          ImGui::CloseCurrentPopup();
        }
      } else if (!snapshot.success) {
        ImGui::TextWrapped(tr_update_failed_fmt.c_str(), snapshot.error.c_str());
        ImGuiStyle &style = ImGui::GetStyle();
        float close_width = ImGui::CalcTextSize(tr_close.c_str()).x + style.FramePadding.x * 2.0f;
        float close_avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (close_avail - close_width) * 0.5f);
        if (ImGui::Button(tr_close.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
          ImGui::CloseCurrentPopup();
        }
      } else {
        int cmp = app_update::compare_versions(kAppVersionTag, snapshot.latest_tag);
        if (cmp < 0) {
          ImGui::TextWrapped(locale.t("update.available_fmt").c_str(), snapshot.latest_tag.c_str(), kAppVersionTag);
#if defined(__linux__)
          ImGui::Spacing();
          ImGui::TextWrapped(locale.t("update.package_manager_note").c_str());
#endif
          ImGuiStyle &style = ImGui::GetStyle();
          const char *yes_label = tr_update_yes_open_link.c_str();
          const char *no_label = tr_update_cancel.c_str();
          (void)style;
          ImGui::Spacing();
          if (ImGui::Button(yes_label, ImVec2(-FLT_MIN, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            app_update::open_url(snapshot.latest_url);
            ImGui::CloseCurrentPopup();
          }
          if (ImGui::Button(no_label, ImVec2(-FLT_MIN, 0))) {
            ImGui::CloseCurrentPopup();
          }
        } else {
          ImGui::TextWrapped(tr_update_up_to_date_fmt.c_str(), kAppVersionTag);
          ImGuiStyle &style = ImGui::GetStyle();
          float ok_width = ImGui::CalcTextSize(tr_ok.c_str()).x + style.FramePadding.x * 2.0f;
          float ok_avail = ImGui::GetContentRegionAvail().x;
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ok_avail - ok_width) * 0.5f);
          if (ImGui::Button(tr_ok.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            ImGui::CloseCurrentPopup();
          }
        }
      }

      ImGui::EndPopup();
    }

    bool open_model_update_popup = false;
    if (!update_popup_visible && settings.auto_update_models && model_updates_available.exchange(false)) {
      open_model_update_popup = true;
    }
    if (open_model_update_popup) {
      {
        std::lock_guard<std::mutex> lock(model_updates_mutex);
        auto_update_ui.updates = model_updates_list;
        model_updates_list.clear();
      }
      auto_update_ui.updating = false;
      auto_update_ui.download_inflight = false;
      auto_update_ui.close_when_done = false;
      auto_update_ui.next_index = 0;
      auto_update_ui.completed = 0;
      auto_update_ui.failed = 0;
      auto_update_ui.last_error.clear();
      auto_update_ui.pending_reload.reset();
      auto_update_ui.active_target_id.reset();
      ImGui::OpenPopup("Model Updates Available");
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginPopupModal("Model Updates Available", nullptr,
             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      int total = static_cast<int>(auto_update_ui.updates.size());
      int done = auto_update_ui.completed + auto_update_ui.failed;

      if (auto_update_ui.close_when_done && !auto_update_ui.updating && !auto_update_ui.download_inflight &&
          total > 0 && done >= total && auto_update_ui.failed == 0) {
        auto_update_ui.close_when_done = false;
        auto_update_ui.updates.clear();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      } else {
      if (auto_update_ui.updating || auto_update_ui.download_inflight) {
        ImGui::TextWrapped("Updating managed models...");
        ImGui::Spacing();
        ImGui::Text("Progress: %d/%d", done, total);
        if (auto_update_ui.active_target_id) {
          ImGui::Text("Downloading: %s", auto_update_ui.active_target_id->c_str());
        }
        if (!auto_update_ui.last_error.empty()) {
          ImGui::Spacing();
          ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", auto_update_ui.last_error.c_str());
        }
      } else if (total <= 0) {
        ImGui::TextWrapped("No managed model updates are currently available.");
      } else {
        ImGui::TextWrapped("There are available updates for one or more managed models.");
        ImGui::Spacing();
        ImGui::Text("Updates available: %d", total);
      }
      ImGui::Separator();
      ImGuiStyle &style = ImGui::GetStyle();
      float update_width = ImGui::CalcTextSize("Update").x + style.FramePadding.x * 2.0f;
      float notnow_width = ImGui::CalcTextSize("Not Now").x + style.FramePadding.x * 2.0f;
      float total_width = update_width + style.ItemSpacing.x + notnow_width;
      float available = ImGui::GetContentRegionAvail().x;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - total_width) * 0.5f);
      bool can_update = total > 0 && !auto_update_ui.updating && !auto_update_ui.download_inflight;
      ImGui::BeginDisabled(!can_update);
      if (ImGui::Button("Update")) {
        auto_update_ui.updating = true;
        auto_update_ui.download_inflight = false;
        auto_update_ui.close_when_done = false;
        auto_update_ui.next_index = 0;
        auto_update_ui.completed = 0;
        auto_update_ui.failed = 0;
        auto_update_ui.last_error.clear();
        auto_update_ui.pending_reload.reset();
        auto_update_ui.active_target_id.reset();
        if (!auto_update_ui.updates.empty()) {
          start_auto_update_download(auto_update_ui.updates[auto_update_ui.next_index++]);
        } else {
          auto_update_ui.updating = false;
        }
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(auto_update_ui.updating || auto_update_ui.download_inflight);
      if (ImGui::Button("Not Now")) {
        auto_update_ui.close_when_done = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::EndPopup();
      }
    }

    float menu_height = ImGui::GetFrameHeight();
    float caption_width = io.DisplaySize.x - (appearance_open ? appearance_panel_width : 0.0f);
    ImGui::SetNextWindowPos(ImVec2(0.0f, menu_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(caption_width, (float)io.DisplaySize.y - menu_height), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, settings.background_color);
    ImGui::PushStyleColor(ImGuiCol_Text, settings.text_color);
    ImGui::Begin("Caption", nullptr, flags);
    ImGui::PushFont(caption_font ? caption_font : ImGui::GetFont());

    bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImGuiIO &local_io = ImGui::GetIO();
    if (auto_scroll_enabled && window_hovered && (std::abs(local_io.MouseWheel) > 0.0f || ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
      auto_scroll_enabled = false;
    }

    std::string composed = caption.buffer();
    if (partial_filtered && !partial_filtered->empty()) {
      composed += *partial_filtered;
    }
    float wrap_width = ImGui::GetContentRegionAvail().x;
    float line_spacing = ImGui::GetTextLineHeight() * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, line_spacing));
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);

    const char *s = composed.c_str();
    const char *e = s + composed.size();
    std::vector<std::string_view> lines;
    lines.reserve(64);
    const char *cur = s;
    while (cur < e) {
      const char *nl = static_cast<const char *>(memchr(cur, '\n', e - cur));
      if (!nl) {
        lines.emplace_back(cur, static_cast<size_t>(e - cur));
        break;
      }
      lines.emplace_back(cur, static_cast<size_t>(nl - cur));
      cur = nl + 1;
    }

    ImVec2 cursor_start = ImGui::GetCursorPos();
    float visible_start = ImGui::GetScrollY();
    float visible_end = visible_start + ImGui::GetWindowHeight();
    float y_offset = 0.0f;
    float item_spacing_y = ImGui::GetStyle().ItemSpacing.y;
    for (const auto &sv : lines) {
      ImVec2 text_size = ImGui::CalcTextSize(sv.data(), sv.data() + sv.size(), false, wrap_width);
      float item_height = text_size.y + item_spacing_y;
      float item_start = y_offset;
      float item_end = y_offset + item_height;
      if (item_end >= visible_start && item_start <= visible_end) {
        ImGui::SetCursorPos(ImVec2(cursor_start.x, cursor_start.y + y_offset));
        ImGui::TextUnformatted(sv.data(), sv.data() + sv.size());
      }
      y_offset += item_height;
      if (item_start > visible_end) {
        break;
      }
    }
    ImGui::SetCursorPos(ImVec2(cursor_start.x, cursor_start.y + y_offset));

    float max_scroll = ImGui::GetScrollMaxY();
    float scroll_y = ImGui::GetScrollY();
    if (auto_scroll_enabled) {
      ImGui::SetScrollY(max_scroll);
    } else if (max_scroll > 0.0f && (max_scroll - scroll_y) < 2.0f) {
      auto_scroll_enabled = true; // user reached bottom, resume auto-scroll
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);

    if (rebuild_caption_font) {
      rebuild_caption_font = false;
      rebuild_fonts();
    }

  }

  audio.stop();
  engine.stop();
  int saved_w = 0;
  int saved_h = 0;
  glfwGetWindowSize(window, &saved_w, &saved_h);
  if (saved_w > 0 && saved_h > 0) {
    settings.window_width = saved_w;
    settings.window_height = saved_h;
  }
  app_update::finalize_update_thread(update_state);
  if (model_update_thread.joinable()) {
    model_update_thread.join();
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  save_settings(settings_path, settings);
  g_file_logger.shutdown();
  return 0;
}

  #if defined(_WIN32)
  int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
      return run_app(0, nullptr);
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(wargc));
    auto wide_to_utf8 = [](const std::wstring &w) {
      if (w.empty()) {
        return std::string();
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
      std::string out(static_cast<size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), size, nullptr, nullptr);
      return out;
    };

    bool want_console = false;
    for (int i = 0; i < wargc; ++i) {
      auto utf8 = wide_to_utf8(wargv[i]);
      if (utf8 == "-console" || utf8 == "--console") {
        want_console = true;
      }
      args.push_back(std::move(utf8));
    }

    LocalFree(wargv);

    if (want_console) {
      AllocConsole();
      FILE *out = nullptr;
      freopen_s(&out, "CONOUT$", "w", stdout);
      freopen_s(&out, "CONOUT$", "w", stderr);
    }

    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &s : args) {
      argv.push_back(s.empty() ? const_cast<char *>("") : s.data());
    }

    return run_app(static_cast<int>(argv.size()), argv.data());
  }

  int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR, int nShow) {
    return wWinMain(hInst, hPrev, GetCommandLineW(), nShow);
  }
  #else
  int main(int argc, char **argv) { return run_app(argc, argv); }
  #endif