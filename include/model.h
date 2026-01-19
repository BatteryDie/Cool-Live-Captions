#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <mutex>

class ModelManager {
public:
  explicit ModelManager(std::filesystem::path base_dir);

  void refresh();
  const std::vector<std::filesystem::path> &models() const { return models_; }

  const std::filesystem::path &user_dir() const { return user_dir_; }

  bool open_models_folder() const;

  struct RemoteModel {
    std::string id;
    std::string version;
    std::string language;
    std::string url;
    std::string filename;
    std::uint64_t size_bytes = 0;
  };

  struct InstalledModel {
    std::string version;
    std::string filename;
  };

  bool fetch_manifest(std::vector<RemoteModel> &out, std::string &error) const;
  bool download_model(const RemoteModel &remote, std::string &error, std::filesystem::path *out_path = nullptr) const;

  std::map<std::string, InstalledModel> installed_models() const {
    std::lock_guard<std::mutex> lock(installed_mutex_);
    return installed_;
  }
  void record_install(const RemoteModel &remote, const std::filesystem::path &local_path);
  void save_installed() const;

private:
  static bool is_supported(const std::filesystem::path &path);
  static std::filesystem::path detect_user_dir();
  static void ensure_dir(const std::filesystem::path &dir);
  void load_installed();
  std::filesystem::path installed_file() const;

  std::vector<std::filesystem::path> models_;
  std::filesystem::path base_dir_;
  std::filesystem::path user_dir_;
  std::map<std::string, InstalledModel> installed_;
  mutable std::mutex installed_mutex_;
};