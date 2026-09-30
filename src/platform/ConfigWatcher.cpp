#include "ConfigWatcher.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <sys/inotify.h>
#include <unistd.h>

namespace skyapo::platform {

ConfigWatcher::ConfigWatcher(const std::filesystem::path &rootConfig) {
  std::error_code error;
  root = std::filesystem::weakly_canonical(rootConfig, error);
  if (error)
    root = std::filesystem::absolute(rootConfig).lexically_normal();
  descriptor = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (descriptor < 0)
    throw std::runtime_error("cannot create config file watcher");
  try {
    update({root});
  } catch (...) {
    close(descriptor);
    descriptor = -1;
    throw;
  }
}

ConfigWatcher::~ConfigWatcher() {
  if (descriptor >= 0)
    close(descriptor);
}

void ConfigWatcher::update(
    const std::vector<std::filesystem::path> &configFiles) {
  std::unordered_map<std::string, DirectoryWatch> desired;
  for (const auto &file : configFiles) {
    std::error_code error;
    auto absolute = std::filesystem::weakly_canonical(file, error);
    if (error)
      absolute = std::filesystem::absolute(file).lexically_normal();
    const auto directory = absolute.parent_path();
    desired[directory.string()].path = directory;
    desired[directory.string()].required = true;
    desired[directory.string()].files.insert(absolute.filename().string());

    auto current = directory;
    while (!current.empty() && current != current.root_path()) {
      const auto parent = current.parent_path();
      if (parent.empty() || parent == current)
        break;
      desired[parent.string()].path = parent;
      desired[parent.string()].directories.insert(current.filename().string());
      current = parent;
    }
  }

  constexpr uint32_t events = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                              IN_MOVED_FROM | IN_ATTRIB | IN_DELETE |
                              IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW;
  std::unordered_map<int, DirectoryWatch> next;
  std::unordered_set<int> retained;
  std::vector<int> added;
  try {
    for (auto &[directory, target] : desired) {
      const auto existing = std::find_if(
          directories.begin(), directories.end(), [&](const auto &entry) {
            return entry.second.path.string() == directory;
          });
      int watch = -1;
      if (existing != directories.end()) {
        watch = existing->first;
        retained.insert(watch);
      } else {
        watch = inotify_add_watch(descriptor, directory.c_str(), events);
        if (watch < 0) {
          if (target.required)
            throw std::runtime_error("cannot watch config directory: " +
                                     directory);
          continue;
        }
        added.push_back(watch);
      }
      next[watch] = std::move(target);
    }
  } catch (...) {
    for (const int watch : added)
      inotify_rm_watch(descriptor, watch);
    throw;
  }

  for (const auto &[watch, directory] : directories)
    if (!retained.count(watch))
      inotify_rm_watch(descriptor, watch);
  directories.swap(next);
}

bool ConfigWatcher::consumeEvents() {
  alignas(inotify_event) char buffer[4096];
  bool relevant = false;
  for (;;) {
    const ssize_t length = read(descriptor, buffer, sizeof(buffer));
    if (length < 0 && errno == EINTR)
      continue;
    if (length <= 0)
      break;
    for (size_t offset = 0; offset < static_cast<size_t>(length);) {
      const auto *event =
          reinterpret_cast<const inotify_event *>(buffer + offset);
      if (event->mask & IN_Q_OVERFLOW) {
        relevant = true;
        for (const auto &[watch, ignored] : directories) {
          (void)ignored;
          inotify_rm_watch(descriptor, watch);
        }
        directories.clear();
      } else if (auto found = directories.find(event->wd);
                 found != directories.end()) {
        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
          relevant = true;
        if (event->len && found->second.files.count(event->name))
          relevant = true;
        if (event->len && found->second.directories.count(event->name) &&
            (event->mask &
             (IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE)))
          relevant = true;
        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
          directories.erase(found);
      }
      offset += sizeof(inotify_event) + event->len;
    }
  }
  return relevant;
}

} // namespace skyapo::platform
