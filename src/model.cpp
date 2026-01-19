#include "model.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <fstream>
#include <sstream>
#include <string_view>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::filesystem::path to_lower_ext(const std::filesystem::path &p) {
  auto ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

std::vector<std::filesystem::path> enumerate(const std::filesystem::path &dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return out;
  }
  for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    out.push_back(entry.path());
  }
  return out;
}

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult run_command(const std::string &cmd) {
  CommandResult res{};
#if defined(_WIN32)
  FILE *pipe = _popen(cmd.c_str(), "r");
#else
  FILE *pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) {
    res.exit_code = -1;
    return res;
  }
  std::array<char, 4096> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    res.output.append(buffer.data());
  }
#if defined(_WIN32)
  res.exit_code = _pclose(pipe);
#else
  res.exit_code = pclose(pipe);
#endif
  return res;
}

std::string extract_json_field(const std::string &json, std::string_view key) {
  auto pos = json.find(key);
  if (pos == std::string::npos) return {};
  pos = json.find('"', json.find(':', pos));
  if (pos == std::string::npos) return {};
  auto end = json.find('"', pos + 1);
  if (end == std::string::npos || end <= pos + 1) return {};
  return json.substr(pos + 1, end - pos - 1);
}

std::uint64_t extract_json_uint(const std::string &json, std::string_view key) {
  auto pos = json.find(key);
  if (pos == std::string::npos) return 0;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return 0;
  auto end = pos + 1;
  while (end < json.size() && std::isspace(static_cast<unsigned char>(json[end]))) {
    ++end;
  }
  std::string number;
  while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
    number.push_back(json[end]);
    ++end;
  }
  if (number.empty()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(number));
  } catch (...) {
    return 0;
  }
}

constexpr const char *kManifestUrl = "https://huggingface.co/batterydie/coollivecaptions-models/resolve/main/manifest.json";
const char *kManagedIndex = "managed_models.txt";

} // namespace

ModelManager::ModelManager(std::filesystem::path base_dir)
    : base_dir_(std::move(base_dir)), user_dir_(detect_user_dir()) {
  ensure_dir(user_dir_);
  load_installed();
}

void ModelManager::refresh() {
  std::vector<std::filesystem::path> collected;
  for (const auto &p : enumerate(user_dir_)) {
    if (is_supported(p)) {
      collected.push_back(p);
    }
  }
  std::sort(collected.begin(), collected.end());
  collected.erase(std::unique(collected.begin(), collected.end()), collected.end());
  models_.swap(collected);
}

bool ModelManager::open_models_folder() const {
  std::filesystem::path target = user_dir_;

#if defined(_WIN32)
  std::wstring wpath = target.wstring();
  auto hinst = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(hinst) > 32;
#elif defined(__APPLE__)
  std::string cmd = "open \"" + target.string() + "\"";
  return std::system(cmd.c_str()) == 0;
#else
  std::string cmd = "xdg-open \"" + target.string() + "\"";
  return std::system(cmd.c_str()) == 0;
#endif
}

bool ModelManager::is_supported(const std::filesystem::path &path) {
  auto ext = to_lower_ext(path);
  return ext == ".april" || ext == ".onnx" || ext == ".ort";
}

std::filesystem::path ModelManager::detect_user_dir() {
#if defined(_WIN32)
  if (const char *localAppData = std::getenv("LOCALAPPDATA")) {
    return std::filesystem::path(localAppData) / "CoolLiveCaptions" / "models";
  }
  if (const char *appData = std::getenv("APPDATA")) {
    return std::filesystem::path(appData) / "CoolLiveCaptions" / "models";
  }
  if (const char *userProfile = std::getenv("USERPROFILE")) {
    return std::filesystem::path(userProfile) / "CoolLiveCaptions" / "models";
  }
#elif defined(__APPLE__)
  if (const char *home = std::getenv("HOME")) {
    return std::filesystem::path(home) / "Library" / "Application Support" / "com.batterydie.coollivecaptions" / "models";
  }
#else
  if (const char *home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".coollivecaptions" / "models";
  }
#endif
  return {};
}

void ModelManager::ensure_dir(const std::filesystem::path &dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
}

std::filesystem::path ModelManager::installed_file() const {
  return user_dir_ / kManagedIndex;
}

void ModelManager::load_installed() {
  std::lock_guard<std::mutex> lock(installed_mutex_);
  installed_.clear();
  auto path = installed_file();
  if (!std::filesystem::exists(path)) {
    return;
  }
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string id, version, filename;
    if (std::getline(ss, id, '|') && std::getline(ss, version, '|') && std::getline(ss, filename, '|')) {
      installed_[id] = InstalledModel{version, filename};
    }
  }
}

void ModelManager::save_installed() const {
  std::lock_guard<std::mutex> lock(installed_mutex_);
  auto path = installed_file();
  std::ofstream out(path, std::ios::trunc);
  for (const auto &kv : installed_) {
    out << kv.first << '|' << kv.second.version << '|' << kv.second.filename << '\n';
  }
}

void ModelManager::record_install(const RemoteModel &remote, const std::filesystem::path &local_path) {
  {
    std::lock_guard<std::mutex> lock(installed_mutex_);
    installed_[remote.id] = InstalledModel{remote.version, local_path.filename().string()};
  }
  save_installed();
}

bool ModelManager::fetch_manifest(std::vector<RemoteModel> &out, std::string &error) const {
  auto res = run_command(std::string("curl -fsSL ") + kManifestUrl);
  if (res.exit_code != 0 || res.output.empty()) {
    error = "Unable to download model manifest.";
    return false;
  }
  out.clear();
  std::size_t pos = 0;
  while (true) {
    auto id_pos = res.output.find("\"id\"", pos);
    if (id_pos == std::string::npos) break;
    auto brace_end = res.output.find('}', id_pos);
    if (brace_end == std::string::npos) break;
    auto chunk = res.output.substr(id_pos, brace_end - id_pos);
    RemoteModel m;
    m.id = extract_json_field(chunk, "\"id\"");
    m.version = extract_json_field(chunk, "\"version\"");
    m.language = extract_json_field(chunk, "\"language\"");
    m.url = extract_json_field(chunk, "\"url\"");
    m.filename = extract_json_field(chunk, "\"filename\"");
    m.size_bytes = extract_json_uint(chunk, "\"size_bytes\"");
    if (!m.id.empty() && !m.url.empty() && !m.filename.empty()) {
      out.push_back(std::move(m));
    }
    pos = brace_end + 1;
  }
  if (out.empty()) {
    error = "Manifest parsed with no models.";
    return false;
  }
  return true;
}

bool ModelManager::download_model(const RemoteModel &remote, std::string &error, std::filesystem::path *out_path) const {
  ensure_dir(user_dir_);
  auto target = user_dir_ / remote.filename;
  auto temp = target;
  temp += ".part";
  std::string cmd = std::string("curl -fL --retry 2 --retry-delay 1 -o \"") + temp.string() + "\" " + remote.url;
  auto res = run_command(cmd);
  if (res.exit_code != 0) {
    error = "Download failed.";
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    error = "Unable to finalize download.";
    return false;
  }
  if (out_path) {
    *out_path = target;
  }
  return true;
}