#pragma once

#include <filesystem>
#include <string>

namespace skyapo::test {

inline std::filesystem::path temporaryPath(const std::string &name) {
  return std::filesystem::temp_directory_path() / name;
}

inline std::string temporaryPattern(const std::string &prefix) {
  return temporaryPath(prefix + "XXXXXX").string();
}

} // namespace skyapo::test
