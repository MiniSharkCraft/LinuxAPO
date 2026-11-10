#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using FilterNew = pw_filter *(*)(pw_core *, const char *, pw_properties *);

bool isSkyapoDaemon() noexcept {
  char executable[256]{};
  const auto length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length <= 0)
    return false;
  const std::string_view path(executable, static_cast<size_t>(length));
  const auto slash = path.find_last_of('/');
  return path.substr(slash == std::string_view::npos ? 0 : slash + 1) ==
         "skyapod";
}

bool triggerFailureOnce() noexcept {
  static std::atomic<bool> failureUsed{false};
  const char *trigger = std::getenv("SKYAPO_TEST_FILTER_FAULT_TRIGGER");
  if (!trigger || access(trigger, F_OK) != 0)
    return false;

  bool expected = false;
  if (!failureUsed.compare_exchange_strong(expected, true,
                                           std::memory_order_relaxed))
    return false;

  const char *receipt = std::getenv("SKYAPO_TEST_FILTER_FAULT_RECEIPT");
  if (receipt) {
    const int descriptor =
        open(receipt, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor >= 0) {
      constexpr char message[] = "injected pw_filter_new failure\n";
      (void)write(descriptor, message, sizeof(message) - 1);
      close(descriptor);
    }
  }
  return true;
}

} // namespace

extern "C" pw_filter *pw_filter_new(pw_core *core, const char *name,
                                    pw_properties *properties) {
  static const auto realFilterNew =
      reinterpret_cast<FilterNew>(dlsym(RTLD_NEXT, "pw_filter_new"));
  if (!realFilterNew) {
    pw_properties_free(properties);
    errno = ENOSYS;
    return nullptr;
  }

  if (isSkyapoDaemon() && properties) {
    const char *nodeName = pw_properties_get(properties, PW_KEY_NODE_NAME);
    if (nodeName && std::strcmp(nodeName, "skyapo.playback.stream") == 0) {
      if (triggerFailureOnce()) {
        pw_properties_free(properties);
        errno = ENOMEM;
        return nullptr;
      }
    }
  }

  return realFilterNew(core, name, properties);
}
