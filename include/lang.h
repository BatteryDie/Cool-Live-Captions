#pragma once

#include <string>
#include <map>
#include <vector>
#include <filesystem>

class LocaleManager {
 public:
  LocaleManager();
  // Load languages from resource paths and load the requested language (fallback to en)
  void load(const std::filesystem::path &exe_path, const std::string &language_code);
  // Returns list of available language codes and display names
  std::vector<std::pair<std::string, std::string>> available_languages() const;
  // Translate a key; returns key if not found
  std::string t(const std::string &key) const;

 private:
  void discover_languages(const std::filesystem::path &dir);
  void load_file(const std::filesystem::path &file);

  std::map<std::string, std::string> translations_;
  std::vector<std::pair<std::string, std::string>> languages_; // code, name
};
