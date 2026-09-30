#include "../src/platform/linux/ConfigSource.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 40> pattern{};
    const std::string prefix = "/tmp/skyapo-config-source-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char *created = mkdtemp(pattern.data());
    if (!created)
      throw std::runtime_error("could not create config-source test directory");
    path = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

bool writeFile(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  return output.good();
}
} // namespace

int main() {
  using skyapo::platform::ConfigSource;
  TemporaryDirectory temporary;
  const auto configDirectory = temporary.path / "configs";
  const auto nestedDirectory = configDirectory / "nested";
  std::filesystem::create_directories(nestedDirectory);
  const auto config = configDirectory / "root.txt";
  const auto nestedConfig = nestedDirectory / "effect.txt";
  if (!writeFile(config, "Include: nested/effect.txt\n") ||
      !writeFile(nestedConfig,
                 "Preamp: -6 dB\n\nFilter: ON PK Fc 100 Hz Gain 2 dB Q 1\n")) {
    std::cerr << "could not write config-source fixture files\n";
    return 1;
  }

  const auto canonical =
      ConfigSource::canonicalize(configDirectory / "../configs/root.txt");
  if (canonical != std::filesystem::weakly_canonical(config) ||
      !canonical.is_absolute()) {
    std::cerr << "config canonicalization did not produce the expected "
                 "absolute path\n";
    return 1;
  }

  if (ConfigSource::resolveInclude(config, "nested/effect.txt") !=
          nestedConfig ||
      ConfigSource::resolveInclude(config, nestedConfig) != nestedConfig) {
    std::cerr << "relative or absolute Include resolution was incorrect\n";
    return 1;
  }

  auto reader = ConfigSource::open(nestedConfig);
  std::array<std::string, 3> lines;
  for (auto &line : lines) {
    if (!reader.readLine(line)) {
      std::cerr << "config reader stopped before the expected end of file\n";
      return 1;
    }
  }
  if (lines[0] != "Preamp: -6 dB" || !lines[1].empty() ||
      lines[2] != "Filter: ON PK Fc 100 Hz Gain 2 dB Q 1" ||
      reader.readLine(lines[0]) || reader.failed()) {
    std::cerr << "config reader changed line boundaries or EOF behavior\n";
    return 1;
  }

  try {
    (void)ConfigSource::open(configDirectory / "missing.txt");
    std::cerr << "opening a missing config unexpectedly succeeded\n";
    return 1;
  } catch (const std::runtime_error &error) {
    if (std::string(error.what()).find("cannot open config:") ==
        std::string::npos) {
      std::cerr << "missing-config diagnostic lost its context: "
                << error.what() << '\n';
      return 1;
    }
  }

  std::cout
      << "Linux config-source path, Include, and line-reader tests passed\n";
}
