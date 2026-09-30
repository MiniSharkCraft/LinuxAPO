#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace skyapo::platform {

// Linux filesystem boundary used by the EAPO configuration loader. Directive
// parsing and filter-factory lifecycle remain in Engine; this adapter owns
// path normalization, Include resolution, and line-oriented file access.
class ConfigSource {
public:
  class Reader {
  public:
    explicit Reader(const std::filesystem::path &path);

    bool readLine(std::string &line);
    bool failed() const noexcept;

  private:
    std::ifstream stream;
  };

  static std::filesystem::path canonicalize(const std::filesystem::path &path);
  static std::filesystem::path
  resolveInclude(const std::filesystem::path &includingFile,
                 const std::filesystem::path &includePath);
  static Reader open(const std::filesystem::path &path);
};

} // namespace skyapo::platform
