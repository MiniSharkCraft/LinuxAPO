#include "../pipewire/Runtime.h"
#include "../platform/Settings.h"
#include "Engine.h"
#include <iostream>
#include <thread>
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
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
    Engine check(48000, 2, 8192);
    check.loadConfig(config);
    if (settings::device().empty())
      throw std::runtime_error(
          "select an input with skyapo device set <id-or-name>");
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    while (!stopping) {
      try {
        runPipeWire(settings::device(), config, stopping);
      } catch (const std::exception &e) {
        std::cerr << "skyapod: " << e.what() << "; retrying in 2 seconds\n";
        for (int i = 0; i < 20 && !stopping; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "skyapod: " << e.what() << '\n';
    return 1;
  }
}
