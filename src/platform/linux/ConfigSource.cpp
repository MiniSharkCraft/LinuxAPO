#include "ConfigSource.h"

#include <stdexcept>
#include <system_error>

namespace skyapo::platform {

ConfigSource::Reader::Reader(const std::filesystem::path &path) : stream(path) {
  if (!stream)
    throw std::runtime_error("cannot open config: " + path.string());
}

bool ConfigSource::Reader::readLine(std::string &line) {
  return static_cast<bool>(std::getline(stream, line));
}

bool ConfigSource::Reader::failed() const noexcept {
  return stream.bad();
}

std::filesystem::path
ConfigSource::canonicalize(const std::filesystem::path &path) {
  std::error_code error;
  auto absolutePath = std::filesystem::absolute(path, error);
  if (error)
    throw std::runtime_error("cannot resolve config path '" + path.string() +
                             "': " + error.message());

  auto normalizedPath = std::filesystem::weakly_canonical(absolutePath, error);
  if (error)
    normalizedPath = absolutePath.lexically_normal();
  return normalizedPath;
}

std::filesystem::path
ConfigSource::resolveInclude(const std::filesystem::path &includingFile,
                             const std::filesystem::path &includePath) {
  if (includePath.is_absolute())
    return includePath;
  return includingFile.parent_path() / includePath;
}

ConfigSource::Reader ConfigSource::open(const std::filesystem::path &path) {
  return Reader(path);
}

} // namespace skyapo::platform
