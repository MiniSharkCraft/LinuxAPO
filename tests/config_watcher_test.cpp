#include "../src/platform/ConfigWatcher.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <cstdlib>
#include <string>
#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace skyapo::platform {
class ConfigWatcherTestAccess {
public:
  static bool injectQueueOverflow(ConfigWatcher &watcher) {
    bool relevant = false;
    watcher.processEvent(-1, IN_Q_OVERFLOW, nullptr, relevant);
    return relevant;
  }
};
} // namespace skyapo::platform

bool write(const fs::path &path, const std::string &contents) {
  std::ofstream output(path);
  output << contents;
  return bool(output);
}

bool hasEvent(skyapo::platform::ConfigWatcher &watcher) {
  pollfd descriptor{watcher.fileDescriptor(), POLLIN, 0};
  return poll(&descriptor, 1, 2000) > 0 && watcher.consumeEvents();
}

int main() {
  char temporary[] = "/tmp/skyapo-config-watcher-XXXXXX";
  const char *directoryName = mkdtemp(temporary);
  if (!directoryName) {
    std::cerr << "could not create config watcher test directory\n";
    return 1;
  }
  const fs::path directory(directoryName);
  const fs::path root = directory / "root.txt";
  const fs::path nestedDirectory = directory / "includes";
  const fs::path included = nestedDirectory / "child.txt";
  fs::create_directories(nestedDirectory);
  if (!write(root, "Include: includes/child.txt\n") ||
      !write(included, "Preamp: -6 dB\n")) {
    std::cerr << "could not write config watcher fixtures\n";
    fs::remove_all(directory);
    return 1;
  }

  int result = 0;
  {
    skyapo::platform::ConfigWatcher watcher(root);
    watcher.update({root, included});
    if (!write(nestedDirectory / "unrelated.txt", "ignored\n") ||
        hasEvent(watcher)) {
      std::cerr << "watcher reacted to an unrelated file\n";
      result = 1;
    }
    if (!write(included, "Preamp: -3 dB\n") || !hasEvent(watcher)) {
      std::cerr << "watcher missed a nested Include edit\n";
      result = 1;
    }
    const auto replacement = nestedDirectory / "child-replacement.txt";
    if (!write(replacement, "Preamp: -4 dB\n")) {
      std::cerr << "could not write atomic-save fixture\n";
      result = 1;
    } else {
      fs::rename(replacement, included);
      if (!hasEvent(watcher)) {
        std::cerr << "watcher missed an atomically replaced Include\n";
        result = 1;
      }
    }

    fs::remove_all(nestedDirectory);
    if (!hasEvent(watcher)) {
      std::cerr << "watcher missed removal of an Include directory\n";
      result = 1;
    }
    fs::create_directories(nestedDirectory);
    if (!write(included, "Preamp: -3 dB\n") || !hasEvent(watcher)) {
      std::cerr << "watcher missed recreation of an Include directory\n";
      result = 1;
    }
    watcher.update({root, included});
    if (!write(included, "Preamp: -4 dB\n") || !hasEvent(watcher)) {
      std::cerr << "watcher did not reattach after Include recreation\n";
      result = 1;
    }

    // Kernel queue overflow is not deterministic to trigger on demand. Route
    // the synthetic overflow through the same event handler, then verify the
    // last-good Include watch still works without calling update().
    if (!skyapo::platform::ConfigWatcherTestAccess::injectQueueOverflow(
            watcher)) {
      std::cerr << "watcher did not request reload after queue overflow\n";
      result = 1;
    }
    if (!write(included, "Preamp: -5 dB\n") || !hasEvent(watcher)) {
      std::cerr << "watcher lost a last-good Include after queue overflow\n";
      result = 1;
    }

    const auto futureDirectory = directory / "future-includes";
    const auto futureInclude = futureDirectory / "child.txt";
    watcher.extend({futureInclude});
    fs::create_directories(futureDirectory);
    if (!hasEvent(watcher)) {
      std::cerr << "watcher missed creation of a missing Include directory\n";
      result = 1;
    }
    watcher.extend({futureInclude});
    if (!write(futureInclude, "Preamp: -4 dB\n") || !hasEvent(watcher)) {
      std::cerr << "watcher missed creation of a future Include file\n";
      result = 1;
    }

    watcher.update({root});
    if (!write(included, "Preamp: -2 dB\n")) {
      std::cerr << "could not update inactive Include fixture\n";
      result = 1;
    } else {
      pollfd descriptor{watcher.fileDescriptor(), POLLIN, 0};
      if (poll(&descriptor, 1, 100) > 0 && watcher.consumeEvents()) {
        std::cerr << "watcher retained a removed Include dependency\n";
        result = 1;
      }
    }
  }

  const fs::path rootAlias = directory / "root-alias.txt";
  if (symlink(root.c_str(), rootAlias.c_str()) != 0) {
    std::cerr << "could not create config watcher symlink fixture\n";
    fs::remove_all(directory);
    return 1;
  }
  {
    skyapo::platform::ConfigWatcher watcher(rootAlias);
    watcher.update({rootAlias, included});
    if (!write(root, "Include: includes/child.txt\n# updated via target\n") ||
        !hasEvent(watcher)) {
      std::cerr << "watcher did not canonicalize a symlink config path\n";
      result = 1;
    }
  }
  fs::remove_all(directory);
  return result;
}
