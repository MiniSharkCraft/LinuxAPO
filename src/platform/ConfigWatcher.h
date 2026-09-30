#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skyapo::platform {

class ConfigWatcher {
public:
  explicit ConfigWatcher(const std::filesystem::path &rootConfig);
  ~ConfigWatcher();
  ConfigWatcher(const ConfigWatcher &) = delete;
  ConfigWatcher &operator=(const ConfigWatcher &) = delete;

  int fileDescriptor() const { return descriptor; }
  void update(const std::vector<std::filesystem::path> &configFiles);
  bool consumeEvents();

private:
  struct DirectoryWatch {
    std::filesystem::path path;
    std::unordered_set<std::string> files;
    std::unordered_set<std::string> directories;
    bool required = false;
  };

  std::filesystem::path root;
  int descriptor = -1;
  std::unordered_map<int, DirectoryWatch> directories;
};

} // namespace skyapo::platform
