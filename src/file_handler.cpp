/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;

    return 0;
  }

  namespace {
    /**
     * @brief Replace every occurrence of `from` with `to` in a small text file, if it exists.
     */
    void rewrite_paths_in_file(const std::filesystem::path &file, const std::string &from, const std::string &to) {
      std::error_code ec;
      if (!std::filesystem::is_regular_file(file, ec)) {
        return;
      }

      std::string contents;
      {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        contents = ss.str();
      }

      bool changed = false;
      for (auto pos = contents.find(from); pos != std::string::npos; pos = contents.find(from, pos + to.size())) {
        contents.replace(pos, from.size(), to);
        changed = true;
      }

      if (changed) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << contents;
      }
    }
  }  // namespace

  std::filesystem::path migrate_config_dir(const std::filesystem::path &legacy_path, const std::filesystem::path &config_path) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (legacy_path == config_path || fs::exists(config_path, ec) || !fs::is_directory(legacy_path, ec)) {
      return config_path;
    }

    std::cout << "Moving config from " << legacy_path << " to " << config_path << std::endl;

    if (config_path.has_parent_path()) {
      fs::create_directories(config_path.parent_path(), ec);
      ec.clear();
    }

    fs::rename(legacy_path, config_path, ec);
    if (ec) {
      // Different filesystems: copy, then remove the original
      ec.clear();
      fs::copy(legacy_path, config_path, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
      if (ec) {
        std::cerr << "Config migration failed: " << ec.message() << ". Still using " << legacy_path << std::endl;
        std::error_code ignored;
        fs::remove_all(config_path, ignored);
        return legacy_path;
      }
      fs::remove_all(legacy_path, ec);
      if (ec) {
        std::cerr << "Copied config, but could not remove " << legacy_path << ": " << ec.message() << std::endl;
      }
    }

    // Covers and other files can be stored with absolute paths
    const auto from = legacy_path.string() + "/";
    const auto to = config_path.string() + "/";
    rewrite_paths_in_file(config_path / "apps.json", from, to);
    rewrite_paths_in_file(config_path / "sunshine.conf", from, to);

    return config_path;
  }
}  // namespace file_handler
