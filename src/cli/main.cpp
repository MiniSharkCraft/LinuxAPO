#include "../platform/Settings.h"
#include "Engine.h"
#ifdef SKYAPO_HAVE_LV2
#include "LV2PluginHost.h"
#endif
#ifdef SKYAPO_HAVE_CLAP
#include "CLAPPluginHost.h"
#endif
#ifdef SKYAPO_HAVE_VST3
#include "VST3PluginHost.h"
#endif
#ifdef SKYAPO_HAVE_PIPEWIRE
#include "../pipewire/DeviceManager.h"
#include <pipewire/version.h>
#endif
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace {
bool daemonReachable() {
  const auto status = settings::queryStatus();
  return !status.empty() &&
         status.find("Daemon: not reachable") == std::string::npos;
}
bool daemonReady() {
  const auto status = settings::queryStatus();
  return status.find("Daemon: streaming") != std::string::npos &&
         status.find("Sample rate: unknown") == std::string::npos &&
         status.find("Quantum: unknown") == std::string::npos;
}
void startDaemon() {
  if (daemonReachable()) {
    std::cout << "skyapod is already running\n";
    return;
  }
  if (settings::device().empty())
    throw std::runtime_error(
        "select an input first: skyapo device set <device>");
  settings::config();
  const pid_t child = fork();
  if (child < 0)
    throw std::runtime_error("cannot fork skyapod");
  if (child == 0) {
    if (setsid() < 0)
      _exit(126);
    const auto log = settings::configDir() / "skyapod.log";
    const int output = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    const int nullInput = open("/dev/null", O_RDONLY);
    if (output >= 0) {
      dup2(output, STDOUT_FILENO);
      dup2(output, STDERR_FILENO);
      if (output > STDERR_FILENO)
        close(output);
    } else {
      const int nullOutput = open("/dev/null", O_WRONLY);
      if (nullOutput >= 0) {
        dup2(nullOutput, STDOUT_FILENO);
        dup2(nullOutput, STDERR_FILENO);
        if (nullOutput > STDERR_FILENO)
          close(nullOutput);
      }
    }
    if (nullInput >= 0) {
      dup2(nullInput, STDIN_FILENO);
      if (nullInput > STDERR_FILENO)
        close(nullInput);
    }
    std::error_code ec;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
      executable = executable.parent_path() / "skyapod";
      execl(executable.c_str(), "skyapod", static_cast<char *>(nullptr));
    }
    execlp("skyapod", "skyapod", static_cast<char *>(nullptr));
    _exit(127);
  }
  for (unsigned attempt = 0; attempt < 50; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (daemonReady()) {
      std::cout << "skyapod started\n";
      return;
    }
  }
  throw std::runtime_error("skyapod did not become reachable within 5 seconds; "
                           "check the per-user skyapod.log");
}
void stopDaemon() {
  if (!daemonReachable()) {
    std::cout << "skyapod is not running\n";
    return;
  }
  std::cout << settings::daemonRequest("STOP\n");
  for (unsigned attempt = 0; attempt < 30; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!daemonReachable())
      return;
  }
  throw std::runtime_error("skyapod did not stop within 3 seconds");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("usage: skyapo status | start | stop | restart "
                               "| device list/set/current | config show/reload "
                               "| config check <file> | plugin list/scan "
                               "| plugin info <URI>");
    std::string cmd = argv[1];
    if (cmd == "status") {
      const auto status = settings::queryStatus();
      if (status.empty())
        throw std::runtime_error(
            "daemon control socket is unresponsive (status request timed out)");
      std::cout << status;
      return 0;
    }
    if (cmd == "diagnostics" && argc == 2) {
      std::cout << "SkyAPO version: " << SKYAPO_VERSION
                << "\nEqualizer APO upstream: " << SKYAPO_UPSTREAM_REVISION
                << '\n';
#ifdef SKYAPO_HAVE_PIPEWIRE
      std::cout << "PipeWire library: " << pw_get_library_version() << '\n';
#else
      std::cout << "PipeWire library: unavailable (not built)\n";
#endif
      const auto status = settings::queryStatus();
      if (status.empty())
        std::cout << "Daemon: unresponsive (status request timed out)\n";
      else
        std::cout << status;
      return 0;
    }
    if (cmd == "filters" && argc == 2) {
      const auto status = settings::queryStatus();
      if (status.empty() ||
          status.find("Daemon: not reachable") != std::string::npos)
        throw std::runtime_error(
            "daemon is unavailable or unresponsive; active filters unavailable");
      const auto begin = status.find("Filter chain:");
      const auto end = status.find("\nConfig:", begin);
      if (begin == std::string::npos)
        throw std::runtime_error("daemon status does not contain filter data");
      std::cout << status.substr(begin, end == std::string::npos
                                            ? std::string::npos
                                            : end - begin)
                << '\n';
      return 0;
    }
    if (cmd == "plugin" && argc == 3 &&
        (std::string(argv[2]) == "list" || std::string(argv[2]) == "scan")) {
      bool foundAnyHost = false;
#ifdef SKYAPO_HAVE_CLAP
      foundAnyHost = true;
      CLAPPluginHost clapHost;
      const auto clapPlugins = clapHost.list();
      std::cout << "CLAP plugins discovered: " << clapPlugins.size() << '\n';
      for (const auto &[id, name] : clapPlugins)
        std::cout << "CLAP\t" << id << '\t' << name << '\n';
#endif
#ifdef SKYAPO_HAVE_VST3
      foundAnyHost = true;
      VST3PluginHost vst3Host;
      const auto vst3Plugins = vst3Host.list();
      std::cout << "VST3 plugins discovered: " << vst3Plugins.size() << '\n';
      for (const auto &[id, name] : vst3Plugins)
        std::cout << "VST3\t" << id << '\t' << name << '\n';
#endif
#ifdef SKYAPO_HAVE_LV2
      foundAnyHost = true;
      LV2PluginHost host;
      const auto plugins = host.list();
      std::cout << "LV2 plugins discovered: " << plugins.size() << '\n';
      for (const auto &[uri, name] : plugins)
        std::cout << "LV2\t" << uri << '\t' << name << '\n';
#endif
      if (foundAnyHost)
        return 0;
      throw std::runtime_error("no plugin host was built");
    }
    if (cmd == "plugin" && argc == 4 && std::string(argv[2]) == "info") {
#ifdef SKYAPO_HAVE_CLAP
      {
        CLAPPluginHost clapHost;
        const std::string requested = argv[3];
        const auto plugins = clapHost.list();
        const auto found = std::find_if(
            plugins.begin(), plugins.end(), [&](const auto &plugin) {
              return plugin.first == requested;
            });
        if (found != plugins.end()) {
          const auto info = clapHost.describe(requested);
          std::cout << info.name << "\nFormat: CLAP\nID: " << info.uri
                    << "\n";
          if (info.inputParameters.empty())
            std::cout << "Input parameters: none\n";
          for (const auto &parameter : info.inputParameters)
            std::cout << parameter.symbol << "\t" << parameter.name << "\t"
                      << "default=" << parameter.defaultValue << "\t"
                      << "range=[" << parameter.minimum << ", "
                      << parameter.maximum << "]\n";
          return 0;
        }
      }
#endif
#ifdef SKYAPO_HAVE_VST3
      {
        const std::string requested = argv[3];
        VST3PluginHost host;
        const auto plugins = host.list();
        const auto found = std::find_if(
            plugins.begin(), plugins.end(), [&](const auto &plugin) {
              return plugin.first == requested;
            });
        if (found != plugins.end()) {
          const auto info = host.describe(requested);
          std::cout << info.name << "\nFormat: VST3\nClass UID: " << info.uri
                    << '\n';
          if (info.inputParameters.empty())
            std::cout << "Input parameters: none\n";
          for (const auto &parameter : info.inputParameters)
            std::cout << parameter.symbol << "\t" << parameter.name << "\t"
                      << "default=" << parameter.defaultValue << "\t"
                      << "range=[" << parameter.minimum << ", "
                      << parameter.maximum << "]\tvalue=" << parameter.value
                      << '\n';
          return 0;
        }
      }
#endif
#ifdef SKYAPO_HAVE_LV2
      const auto info = LV2PluginHost().describe(argv[3]);
      std::cout << info.name << "\nURI: " << info.uri << '\n';
      if (info.inputParameters.empty())
        std::cout << "Input control parameters: none\n";
      for (const auto &parameter : info.inputParameters)
        std::cout << parameter.symbol << "\t" << parameter.name << "\t"
                  << "default=" << parameter.defaultValue << "\t"
                  << "range=[" << parameter.minimum << ", " << parameter.maximum
                  << "]\n";
      return 0;
#else
#ifdef SKYAPO_HAVE_CLAP
      throw std::runtime_error("plugin not found in CLAP catalog");
#else
      throw std::runtime_error("LV2 support was not built (install Lilv)");
#endif
#endif
    }
    if (cmd == "start" && argc == 2) {
      startDaemon();
      return 0;
    }
    if (cmd == "stop" && argc == 2) {
      stopDaemon();
      return 0;
    }
    if (cmd == "restart" && argc == 2) {
      stopDaemon();
      startDaemon();
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
        std::cout
            << "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\n";
        for (auto &d : ds.sources)
          if (d.name != "skyapo.virtual_mic")
            std::cout << d.id << '\t' << d.name << '\t' << d.description << '\t'
                      << (d.name == settings::device() ? "yes" : "") << '\t'
                      << (d.channels ? std::to_string(d.channels) : "unknown")
                      << '\t'
                      << (d.sampleRate ? std::to_string(d.sampleRate)
                                       : "unknown")
                      << '\n';
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
    if (cmd == "config" && argc == 3 && std::string(argv[2]) == "show") {
      std::ifstream config(settings::config());
      std::cout << config.rdbuf();
      if (!config)
        throw std::runtime_error("cannot read config");
      return 0;
    }
    if (cmd == "config" && argc == 3 && std::string(argv[2]) == "reload") {
      std::cout << settings::daemonRequest("RELOAD\n");
      return 0;
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
