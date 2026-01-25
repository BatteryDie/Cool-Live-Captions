#include "model.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string_view>
#include <mutex>
#include <curl/curl.h>

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

static size_t write_callback_string(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  auto *str = static_cast<std::string *>(userp);
  str->append(static_cast<char *>(contents), total);
  return total;
}

static size_t write_callback_file(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  auto *file = static_cast<FILE *>(userp);
  return fwrite(contents, 1, total, file);
}

bool download_to_string(const std::string &url, std::string &out, std::string &error) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "Failed to initialize libcurl.";
    return false;
  }
  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK) {
    error = std::string("Download failed: ") + curl_easy_strerror(res);
    return false;
  }
  return true;
}

bool download_to_file(const std::string &url, const std::filesystem::path &path, std::string &error) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "Failed to initialize libcurl.";
    return false;
  }
  FILE *fp = fopen(path.string().c_str(), "wb");
  if (!fp) {
    error = "Failed to open file for writing.";
    curl_easy_cleanup(curl);
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  CURLcode res = curl_easy_perform(curl);
  fclose(fp);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK) {
    error = std::string("Download failed: ") + curl_easy_strerror(res);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
  }
  return true;
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

constexpr const char *kDefaultManifestUrl = "https://huggingface.co/batterydie/coollivecaptions-models/resolve/main/manifest.json";
constexpr const char *kDevManifestUrl = "http://localhost:8000/manifest.json";
const char *kManagedIndex = "managed_models.json";

} // namespace

ModelManager::ModelManager(std::filesystem::path base_dir, bool use_dev_manifest)
    : base_dir_(std::move(base_dir)), user_dir_(detect_user_dir()), use_dev_manifest_(use_dev_manifest) {
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
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::size_t pos = 0;
  while (true) {
    auto id_pos = json.find("\"id\"", pos);
    if (id_pos == std::string::npos) break;
    auto brace_begin = json.rfind('{', id_pos);
    if (brace_begin == std::string::npos) brace_begin = id_pos;
    auto brace_end = json.find('}', id_pos);
    if (brace_end == std::string::npos) break;
    auto chunk = json.substr(brace_begin, brace_end - brace_begin + 1);
    std::string id = extract_json_field(chunk, "\"id\"");
    std::string version = extract_json_field(chunk, "\"version\"");
    std::string filename = extract_json_field(chunk, "\"filename\"");
    if (!id.empty() && !filename.empty()) {
      installed_[id] = InstalledModel{version, filename};
    }
    pos = brace_end + 1;
  }
}

void ModelManager::save_installed() const {
  std::lock_guard<std::mutex> lock(installed_mutex_);
  auto path = installed_file();
  auto tmp = path;
  tmp += ".tmp";
  std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
  if (!out) {
    return;
  }
  out << "[\n";
  bool first = true;
  for (const auto &kv : installed_) {
    if (!first) out << ",\n";
    first = false;
    // Write a minimal JSON object for each installed model
    auto escape = [](const std::string &s) {
      std::string r;
      for (unsigned char c : s) {
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\b': r += "\\b"; break;
        case '\f': r += "\\f"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            r += buf;
          } else {
            r.push_back(static_cast<char>(c));
          }
        }
      }
      return r;
    };
    out << "  {\"id\":\"" << escape(kv.first) << "\",\"version\":\"" << escape(kv.second.version)
        << "\",\"filename\":\"" << escape(kv.second.filename) << "\"}";
  }
  out << "\n]\n";
  out.close();
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
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
  std::string json;
  if (use_dev_manifest_) {
    if (!download_to_string(kDevManifestUrl, json, error)) {
      return false;
    }
  } else {
    if (!download_to_string(kDefaultManifestUrl, json, error)) {
      return false;
    }
  }
  if (json.empty()) {
    error = "Downloaded manifest is empty.";
    return false;
  }
  out.clear();
  std::size_t pos = 0;
  while (true) {
    auto id_pos = json.find("\"id\"", pos);
    if (id_pos == std::string::npos) break;
    auto brace_end = json.find('}', id_pos);
    if (brace_end == std::string::npos) break;
    auto chunk = json.substr(id_pos, brace_end - id_pos);
    RemoteModel m;
    m.id = extract_json_field(chunk, "\"id\"");
    m.version = extract_json_field(chunk, "\"version\"");
    m.language = extract_json_field(chunk, "\"language\"");
    m.url = extract_json_field(chunk, "\"url\"");
    m.filename = extract_json_field(chunk, "\"filename\"");
    m.size_bytes = extract_json_uint(chunk, "\"size_bytes\"");
    m.name = extract_json_field(chunk, "\"name\"");
    m.author = extract_json_field(chunk, "\"author\"");
    m.description = extract_json_field(chunk, "\"description\"");
    m.url_website = extract_json_field(chunk, "\"url_website\"");
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
  
  if (!download_to_file(remote.url, temp, error)) {
    return false;
  }
  
  std::error_code ec;
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    error = "Unable to finalize download.";
    std::filesystem::remove(temp, ec);
    return false;
  }
  if (out_path) {
    *out_path = target;
  }
  return true;
}

bool ModelManager::remove_installed(const std::string &id, std::string &error) {
  std::string filename;
  {
    std::lock_guard<std::mutex> lock(installed_mutex_);
    auto it = installed_.find(id);
    if (it == installed_.end()) {
      error = "Model not found in installed index.";
      return false;
    }
    // Remove from index first and persist immediately to avoid races where the file is removed
    // externally or concurrently.
    filename = it->second.filename;
    installed_.erase(it);
  }

  // Persist the updated index outside the lock.
  save_installed();

  std::filesystem::path file = user_dir_ / filename;
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    std::filesystem::remove(file, ec);
    if (ec) {
      // File removal failed, but the index was already updated. Report the error but consider
      // the operation successful from the user's perspective.
      error = "Unable to remove model file: " + ec.message();
      return true;
    }
  }
  return true;
}