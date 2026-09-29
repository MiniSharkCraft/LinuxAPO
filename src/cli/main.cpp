#include "../platform/Settings.h"
#include "Engine.h"
#ifdef SKYAPO_HAVE_PIPEWIRE
#include "../pipewire/DeviceManager.h"
#endif
#include <iostream>
int main(int argc, char **argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("usage: skyapo status | device list/set/current "
                               "| config check <file>");
    std::string cmd = argv[1];
    if (cmd == "status") {
      std::cout << settings::queryStatus();
      return 0;
    }
    if (cmd == "device" && argc >= 3) {
      std::string action = argv[2];
      if (action == "current") {
        auto d = settings::device();
        std::cout << (d.empty() ? "No input selected" : d) << '\n';
        return 0;
      }
#ifdef SKYAPO_HAVE_PIPEWIRE
      auto ds = enumerateDevices();
      if (action == "list") {
        std::cout << "ID\tNODE NAME\tDESCRIPTION\tSELECTED\n";
        for (auto &d : ds.sources)
          if (d.name != "skyapo.virtual_mic")
            std::cout << d.id << '\t' << d.name << '\t' << d.description << '\t'
                      << (d.name == settings::device() ? "yes" : "") << '\n';
        return 0;
      }
      if (action == "set" && argc == 4) {
        for (auto &d : ds.sources)
          if ((d.name == argv[3] || std::to_string(d.id) == argv[3]) &&
              d.name != "skyapo.virtual_mic") {
            settings::select(d.name);
            std::cout << "Selected " << d.name
                      << " (running daemon applies it shortly)\n";
            return 0;
          }
        throw std::runtime_error("capture device not found");
      }
#else
      throw std::runtime_error("PipeWire support unavailable at build time");
#endif
    }
    if (cmd == "config" && argc == 4 && std::string(argv[2]) == "check") {
      Engine e(48000, 2, 8192);
      e.loadConfig(argv[3]);
      std::cout << "Valid config: " << e.filterCount() << " filters\n";
      return 0;
    }
    throw std::runtime_error("unknown command or invalid arguments");
  } catch (const std::exception &e) {
    std::cerr << "skyapo: " << e.what() << '\n';
    return 1;
  }
}
