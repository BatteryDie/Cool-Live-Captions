#include "app_update.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <curl/curl.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace app_update {
namespace {

constexpr const char *kGithubRepo = GITHUB_REPO;

// libcurl callback to write data to a string
static size_t write_callback_string(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  auto *str = static_cast<std::string *>(userp);
  str->append(static_cast<char *>(contents), total);
  return total;
}

// Download URL to string using libcurl (no terminal popup)
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
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "CoolLiveCaptions/1.0");
  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK) {
    error = std::string("Download failed: ") + curl_easy_strerror(res);
    return false;
  }
  return true;
}

std::string extract_json_field(const std::string &json, std::string_view key) {
  auto pos = json.find(key);
  if (pos == std::string::npos) {
    return {};
  }
  pos = json.find('"', json.find(':', pos));
  if (pos == std::string::npos) {
    return {};
  }
  auto end = json.find('"', pos + 1);
  if (end == std::string::npos || end <= pos + 1) {
    return {};
  }
  return json.substr(pos + 1, end - pos - 1);
}

UpdateResult fetch_latest_release() {
  UpdateResult r{};
  std::string api = std::string("https://api.github.com/repos/") + kGithubRepo + "/releases/latest";
  std::string output;
  std::string error;
  if (!download_to_string(api, output, error) || output.empty()) {
    std::fprintf(stderr, "[error] Update check failed: unable to reach GitHub releases (network/offline)\n");
    r.error = error.empty() ? "Unable to reach GitHub releases." : error;
    return r;
  }
  r.latest_tag = extract_json_field(output, "\"tag_name\"");
  r.latest_url = extract_json_field(output, "\"html_url\"");
  if (r.latest_tag.empty() || r.latest_url.empty()) {
    std::fprintf(stderr, "[error] Update check failed: unexpected release response (parse)\n");
    r.error = "Unable to parse release response.";
    return r;
  }
  r.success = true;
  return r;
}

}  // namespace

int compare_versions(const std::string &a, const std::string &b) {
  auto trim_v = [](std::string s) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
      s.erase(s.begin());
    }
    return s;
  };
  auto sa = trim_v(a);
  auto sb = trim_v(b);
  auto split = [](const std::string &s) {
    std::vector<int> parts;
    std::string token;
    for (char c : s) {
      if (c == '.') {
        if (!token.empty()) {
          parts.push_back(std::stoi(token));
          token.clear();
        }
      } else if (std::isdigit(static_cast<unsigned char>(c))) {
        token.push_back(c);
      }
    }
    if (!token.empty()) {
      parts.push_back(std::stoi(token));
    }
    return parts;
  };
  auto va = split(sa);
  auto vb = split(sb);
  size_t count = std::max(va.size(), vb.size());
  va.resize(count, 0);
  vb.resize(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (va[i] < vb[i]) return -1;
    if (va[i] > vb[i]) return 1;
  }
  return 0;
}

bool open_url(const std::string &url) {
#if defined(_WIN32)
  auto wurl = std::wstring(url.begin(), url.end());
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
  return std::system((std::string("open \"") + url + "\"").c_str()) == 0;
#else
  return std::system((std::string("xdg-open \"") + url + "\" 2>/dev/null").c_str()) == 0;
#endif
}

void start_update_check(UpdateState &state, bool show_modal) {
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.checking) {
      if (show_modal) {
        state.show_modal = true;
      }
      return;
    }
    if (state.worker.joinable()) {
      state.worker.join();
    }
    state.show_modal = show_modal;
    state.checking = true;
    state.has_result = false;
    state.result = UpdateResult{};
  }

  state.worker = std::thread([&state]() {
    UpdateResult r = fetch_latest_release();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.result = std::move(r);
    state.has_result = true;
    state.checking = false;
  });
}

void finalize_update_thread(UpdateState &state) {
  std::thread joiner;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.checking && state.worker.joinable()) {
      joiner = std::move(state.worker);
    }
  }
  if (joiner.joinable()) {
    joiner.join();
  }
}

}  // namespace app_update
