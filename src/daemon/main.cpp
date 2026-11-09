#include "../pipewire/Runtime.h"
#include "../platform/Settings.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
} // namespace
int main(int argc, char **argv) {
  try {
    settings::DaemonLock lock;
    std::string config;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--config" && i + 1 < argc)
        config = argv[++i];
      else
        throw std::runtime_error("usage: skyapod [--config config.txt]");
    }
    if (config.empty())
      config = settings::config();
    config = std::filesystem::absolute(config).string();
    if (settings::deviceSelection().name.empty())
      throw std::runtime_error(
          "select an input with skyapo device set <id-or-name>");
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    while (!stopping) {
      try {
        const auto selection = settings::deviceSelection();
        runPipeWire(selection.name, selection.stableProperty, config, stopping);
      } catch (const std::exception &e) {
        const char *retryGate = std::getenv("SKYAPO_TEST_RETRY_GATE_FILE");
        if (retryGate && retryGate[0] != '\0') {
          std::cerr << "skyapod: " << e.what()
                    << "; retry paused by test gate\n";
          const std::filesystem::path gatePath(retryGate);
          std::error_code error;
          while (!stopping && !std::filesystem::exists(gatePath, error)) {
            if (error)
              throw std::runtime_error("cannot inspect test retry gate: " +
                                       error.message());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        } else {
          std::cerr << "skyapod: " << e.what() << "; retrying in 2 seconds\n";
          for (int i = 0; i < 20 && !stopping; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "skyapod: " << e.what() << '\n';
    return 1;
  }
}
