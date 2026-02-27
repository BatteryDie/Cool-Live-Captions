#include "lang.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

LocaleManager::LocaleManager() {}

static std::string trim(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

void LocaleManager::discover_languages(const std::filesystem::path &dir) {
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return;
  for (auto &ent : std::filesystem::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    auto p = ent.path();
    if (p.extension() == ".xml") {
      // Expect filename like en.xml or en-US.xml
      std::string code = p.stem().string();
      std::string name = code;
      // Try to read language name from file
      std::ifstream in(p);
      std::string all;
      std::getline(in, all, '\0');
      auto pos = all.find("<language");
      if (pos != std::string::npos) {
        auto name_pos = all.find("name=\"", pos);
        if (name_pos != std::string::npos) {
          name_pos += 6;
          auto end = all.find('"', name_pos);
          if (end != std::string::npos) {
            name = all.substr(name_pos, end - name_pos);
            name = trim(name);
          }
        }
      }
      languages_.push_back({code, name});
    }
  }
}

void LocaleManager::load_file(const std::filesystem::path &file) {
  translations_.clear();
  std::ifstream in(file);
  if (!in) return;
  std::string line;
  std::string content;
  while (std::getline(in, line)) content += line + "\n";
  // Very small XML-ish parser: find <string id="key">value</string>
  size_t pos = 0;
  while (true) {
    auto s = content.find("<string", pos);
    if (s == std::string::npos) break;
    auto id_pos = content.find("id=\"", s);
    if (id_pos == std::string::npos) break;
    id_pos += 4;
    auto id_end = content.find('"', id_pos);
    if (id_end == std::string::npos) break;
    std::string id = content.substr(id_pos, id_end - id_pos);
    auto start = content.find('>', id_end);
    if (start == std::string::npos) break;
    start += 1;
    auto end_tag = content.find("</string>", start);
    if (end_tag == std::string::npos) break;
    std::string val = content.substr(start, end_tag - start);
    // unescape minimal
    auto replace_all = [&](std::string &s, const std::string &a, const std::string &b) {
      size_t p = 0;
      while ((p = s.find(a, p)) != std::string::npos) {
        s.replace(p, a.size(), b);
        p += b.size();
      }
    };
    replace_all(val, "&lt;", "<");
    replace_all(val, "&gt;", ">");
    replace_all(val, "&amp;", "&");
    // unescape common backslash sequences so translations can include \n, \t, etc.
    replace_all(val, "\\\\", "\\");
    replace_all(val, "\\n", "\n");
    replace_all(val, "\\r", "\r");
    replace_all(val, "\\t", "\t");
    translations_[trim(id)] = trim(val);
    pos = end_tag + 9;
  }
}

void LocaleManager::load(const std::filesystem::path &exe_path, const std::string &language_code) {
  languages_.clear();
  translations_.clear();
  std::vector<std::filesystem::path> candidates = {
    exe_path / "lang",
    exe_path / "resources" / "lang",
    exe_path.parent_path() / "resources" / "lang",
    std::filesystem::path("/usr/share/coollivecaptions/resources/lang"),
    std::filesystem::path("/opt/coollivecaptions/resources/lang")
  };
  for (const auto &c : candidates) discover_languages(c);
  // ensure unique
  std::sort(languages_.begin(), languages_.end());
  languages_.erase(std::unique(languages_.begin(), languages_.end()), languages_.end());

  // try to find file for requested language
  std::filesystem::path found;
  for (const auto &c : candidates) {
    auto p = c / (language_code + std::string(".xml"));
    if (std::filesystem::exists(p)) {
      found = p;
      break;
    }
  }
  if (found.empty()) {
    // fallback to en
    for (const auto &c : candidates) {
      auto p = c / "en.xml";
      if (std::filesystem::exists(p)) {
        found = p;
        break;
      }
    }
  }
  if (!found.empty()) {
    load_file(found);
  }
}

std::vector<std::pair<std::string, std::string>> LocaleManager::available_languages() const {
  return languages_;
}

std::string LocaleManager::t(const std::string &key) const {
  auto it = translations_.find(key);
  if (it != translations_.end()) return it->second;
  return key;
}
