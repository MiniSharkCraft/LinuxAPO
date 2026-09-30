#include "../platform/Settings.h"
#include "Engine.h"
#include "helpers/StringHelper.h"
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
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>
namespace {
std::string jsonString(const std::string &value) {
  std::ostringstream out;
  out << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < value.size(); ++i) {
    const auto ch = static_cast<unsigned char>(value[i]);
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
      } else if (ch >= 0x80) {
        size_t length = 0;
        if (ch >= 0xc2 && ch <= 0xdf)
          length = 2;
        else if (ch >= 0xe0 && ch <= 0xef)
          length = 3;
        else if (ch >= 0xf0 && ch <= 0xf4)
          length = 4;
        bool valid = length && i + length <= value.size();
        for (size_t j = 1; valid && j < length; ++j) {
          const auto continuation =
              static_cast<unsigned char>(value[i + j]);
          valid = continuation >= 0x80 && continuation <= 0xbf;
          if (j == 1) {
            if (ch == 0xe0)
              valid = continuation >= 0xa0 && continuation <= 0xbf;
            else if (ch == 0xed)
              valid = continuation >= 0x80 && continuation <= 0x9f;
            else if (ch == 0xf0)
              valid = continuation >= 0x90 && continuation <= 0xbf;
            else if (ch == 0xf4)
              valid = continuation >= 0x80 && continuation <= 0x8f;
          }
        }
        if (valid) {
          out.write(value.data() + i, static_cast<std::streamsize>(length));
          i += length - 1;
        } else {
          out << "\\ufffd";
        }
      } else {
        out << static_cast<char>(ch);
      }
    }
  }
  out << '"';
  return out.str();
}

std::optional<std::string> statusValue(const std::string &status,
                                       const std::string &label) {
  const std::string prefix = label + ": ";
  size_t begin = 0;
  while (begin < status.size()) {
    const size_t end = status.find('\n', begin);
    const auto line = status.substr(begin, end == std::string::npos
                                               ? std::string::npos
                                               : end - begin);
    if (line.rfind(prefix, 0) == 0)
      return line.substr(prefix.size());
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return std::nullopt;
}

std::wstring configDeviceMatchText() {
  const auto selected = settings::device();
  if (selected.empty())
    return {};
  // Keep config validation independent of a live PipeWire registry connection.
  // Runtime matching uses the complete discovered identity metadata.
  return L"node.name=" + StringHelper::toWString(selected, 65001);
}

std::optional<unsigned long long> unsignedValue(const std::string &value) {
  if (value.empty())
    return std::nullopt;
  char *end = nullptr;
  const auto number = std::strtoull(value.c_str(), &end, 10);
  if (end == value.c_str() || (*end && *end != ' '))
    return std::nullopt;
  return number;
}

std::optional<double> decimalValue(const std::string &value) {
  if (value.empty())
    return std::nullopt;
  char *end = nullptr;
  const auto number = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || !std::isfinite(number))
    return std::nullopt;
  return number;
}

std::optional<double> statusNumber(const std::string &status,
                                   const std::string &label) {
  const auto value = statusValue(status, label);
  return value ? decimalValue(*value) : std::nullopt;
}

void writeJsonUnsigned(std::ostream &out,
                       const std::optional<unsigned long long> &value);

std::optional<unsigned long long> statusUnsigned(const std::string &status,
                                                 const std::string &label) {
  const auto value = statusValue(status, label);
  return value ? unsignedValue(*value) : std::nullopt;
}

std::optional<std::vector<std::string>> statusListAfter(
    const std::string &status, const std::string &label) {
  const std::string marker = "\n" + label + ":";
  const auto markerPosition = status.find(marker);
  if (markerPosition == std::string::npos)
    return std::nullopt;
  size_t begin = markerPosition + marker.size();
  if (status.compare(begin, 8, " (none)\n") == 0)
    return std::vector<std::string>{};
  if (begin < status.size() && status[begin] == '\n')
    ++begin;
  std::vector<std::string> values;
  while (begin < status.size()) {
    const auto end = status.find('\n', begin);
    const auto line = status.substr(begin, end == std::string::npos
                                               ? std::string::npos
                                               : end - begin);
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos)
      values.push_back(line.substr(first));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return values;
}

void printUsage(std::ostream &out) {
  out << "Usage: skyapo <command> [arguments]\n"
         "\n"
         "Commands:\n"
         "  status                         Show daemon and audio graph status\n"
         "  diagnostics [--json]           Show build/runtime diagnostics\n"
         "  start | stop | restart         Control the per-user daemon\n"
         "  device list                    List PipeWire capture devices\n"
         "  device set <id-or-name>        Select a capture device\n"
         "  device current                 Show the selected capture device\n"
         "  config show                    Print the active configuration\n"
         "  config check <file>            Validate a configuration\n"
         "  config check --json <file>     Validate and emit JSON diagnostics\n"
         "  config reload                  Reload the active configuration\n"
         "  filters                        Show active filters\n"
         "  plugin list                    List discovered plugins\n"
         "  plugin set <PLUGIN-ID> <param> <value>  Set a live plugin parameter\n"
         "  plugin bypass <PLUGIN-ID> on|off        Set host-level bypass\n"
         "  plugin info <URI>              Show plugin metadata\n"
         "  help                           Show this help\n"
         "  --version                      Show SkyAPO and upstream versions\n";
}

std::string trimText(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

struct ConfigDiagnostic {
  std::string file;
  std::optional<unsigned long long> line;
  std::optional<std::string> directive;
  std::optional<std::string> command;
  std::string reason;
  std::vector<Engine::IncludeSite> includeChain;
};

ConfigDiagnostic parseConfigDiagnostic(const std::string &message,
                                       const std::string &requestedFile) {
  ConfigDiagnostic diagnostic{requestedFile, std::nullopt, std::nullopt,
                              std::nullopt, message, {}};
  const std::string openError = "cannot open config: ";
  if (message.rfind(openError, 0) == 0) {
    diagnostic.file = message.substr(openError.size());
    diagnostic.reason = "cannot open config file";
    return diagnostic;
  }
  size_t marker = std::string::npos;
  size_t lineBegin = 0;
  size_t lineEnd = 0;
  for (size_t i = 0; i < message.size(); ++i) {
    if (message[i] != ':')
      continue;
    size_t digit = i + 1;
    while (digit < message.size() && message[digit] >= '0' &&
           message[digit] <= '9')
      ++digit;
    if (digit == i + 1 || digit >= message.size() || message[digit] != ':' ||
        (digit + 1 < message.size() && message[digit + 1] != ' '))
      continue;
    marker = i;
    lineBegin = i + 1;
    lineEnd = digit;
  }
  if (marker == std::string::npos)
    return diagnostic;

  diagnostic.file = message.substr(0, marker);
  diagnostic.line = unsignedValue(message.substr(lineBegin, lineEnd - lineBegin));
  size_t reasonBegin = lineEnd + 1;
  while (reasonBegin < message.size() && message[reasonBegin] == ' ')
    ++reasonBegin;
  diagnostic.reason = message.substr(reasonBegin);

  if (diagnostic.line && *diagnostic.line <= std::numeric_limits<unsigned>::max()) {
    std::ifstream input(diagnostic.file);
    std::string line;
    for (unsigned long long current = 1; current <= *diagnostic.line; ++current)
      if (!std::getline(input, line))
        break;
    line = trimText(line);
    const auto colon = line.find(':');
    if (!line.empty())
      diagnostic.directive = line;
    if (colon != std::string::npos)
      diagnostic.command = trimText(line.substr(0, colon));
  }
  return diagnostic;
}

void writeConfigCheckJson(const std::string &file, Engine *engine,
                          const std::optional<ConfigDiagnostic> &diagnostic) {
  std::cout << "{\n  \"valid\": " << (engine ? "true" : "false")
            << ",\n  \"file\": " << jsonString(file)
            << ",\n  \"filter_count\": ";
  if (engine)
    std::cout << engine->filterCount();
  else
    std::cout << "null";
  std::cout << ",\n  \"diagnostics\": [";
  if (diagnostic) {
    std::cout << "\n    {\n      \"file\": "
              << jsonString(diagnostic->file) << ",\n      \"line\": ";
    writeJsonUnsigned(std::cout, diagnostic->line);
    std::cout << ",\n      \"directive\": ";
    if (diagnostic->directive)
      std::cout << jsonString(*diagnostic->directive);
    else
      std::cout << "null";
    std::cout << ",\n      \"command\": ";
    if (diagnostic->command)
      std::cout << jsonString(*diagnostic->command);
    else
      std::cout << "null";
    std::cout << ",\n      \"reason\": " << jsonString(diagnostic->reason)
              << ",\n      \"include_chain\": [";
    for (size_t i = 0; i < diagnostic->includeChain.size(); ++i) {
      const auto &site = diagnostic->includeChain[i];
      std::cout << (i ? "," : "") << "\n        {\"file\": "
                << jsonString(site.file.string()) << ", \"line\": "
                << site.line << "}";
    }
    if (!diagnostic->includeChain.empty())
      std::cout << "\n      ";
    std::cout << "]\n    }\n  ";
  }
  std::cout << "]\n}\n";
}

void writeJsonNumber(std::ostream &out, const std::optional<double> &value) {
  if (value)
    out << std::setprecision(17) << *value;
  else
    out << "null";
}

void writeJsonUnsigned(std::ostream &out,
                       const std::optional<unsigned long long> &value) {
  if (value)
    out << *value;
  else
    out << "null";
}

void writeDiagnosticsJson(const std::string &status) {
  const auto daemon = statusValue(status, "Daemon");
  const auto configuredDevice = settings::device();
  const auto selectedDevice = statusValue(status, "Selected device");
  const auto capture = statusValue(status, "Capture node");
  const auto virtualMic = statusValue(status, "Virtual microphone");
  const auto virtualNode = statusValue(status, "Virtual node");
  const auto format = statusValue(status, "Format");
  const auto channelPositions = statusValue(status, "Channel positions");
  const auto channels = statusUnsigned(status, "Channels");
  const auto sampleRate = statusUnsigned(status, "Sample rate");
  const auto quantum = statusUnsigned(status, "Quantum");
  const auto filters = statusUnsigned(status, "Filters");
  const auto pluginLatency =
      statusUnsigned(status, "Plugin-reported latency sum");
  const auto blocks = statusUnsigned(status, "Processed blocks");
  const auto overruns = statusUnsigned(status, "Overruns");
  const auto activeLinks = statusValue(status, "Active capture links");
  const auto average = statusNumber(status, "Process average");
  const auto maximum = statusNumber(status, "Process maximum");
  const auto inputRms = statusNumber(status, "Input RMS");
  const auto outputRms = statusNumber(status, "Output RMS");
  const auto amplitudeRatio = statusNumber(status, "DSP amplitude ratio");
  const auto pluginFailures = statusListAfter(status, "Plugin failures");
  const auto config = statusValue(status, "Config");
  const auto state = daemon ? *daemon : "unresponsive";

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\n  \"skyapo_version\": " << jsonString(SKYAPO_VERSION)
      << ",\n  \"equalizer_apo_upstream\": "
      << jsonString(SKYAPO_UPSTREAM_REVISION) << ",\n  \"pipewire_library\": ";
#ifdef SKYAPO_HAVE_PIPEWIRE
  out << jsonString(pw_get_library_version());
#else
  out << "null";
#endif
  out << ",\n  \"daemon_state\": " << jsonString(state)
      << ",\n  \"selected_device\": {\n    \"configured_identifier\": ";
  if (configuredDevice.empty())
    out << "null";
  else
    out << jsonString(configuredDevice);
  out << ",\n    \"runtime_name\": ";
  if (selectedDevice)
    out << jsonString(*selectedDevice);
  else
    out << "null";
  out << "\n  },\n  \"capture_node\": ";
  if (capture) {
    const auto open = capture->find(" (");
    const auto id = unsignedValue(capture->substr(0, open));
    const auto description =
        open != std::string::npos && capture->size() > open + 3 &&
                capture->back() == ')'
            ? std::optional<std::string>(
                  capture->substr(open + 2, capture->size() - open - 3))
            : std::nullopt;
    out << "{\"id\": ";
    writeJsonUnsigned(out, id);
    out << ", \"description\": ";
    if (description)
      out << jsonString(*description);
    else
      out << "null";
    out << '}';
  } else {
    out << "null";
  }
  out << ",\n  \"virtual_microphone\": ";
  if (virtualMic || virtualNode) {
    out << "{\"name\": ";
    if (virtualMic)
      out << jsonString(*virtualMic);
    else
      out << "null";
    out << ", \"node\": ";
    if (virtualNode) {
      const auto open = virtualNode->find(" (");
      const auto id = unsignedValue(virtualNode->substr(0, open));
      out << "{\"id\": ";
      writeJsonUnsigned(out, id);
      out << ", \"name\": ";
      if (open != std::string::npos && virtualNode->back() == ')')
        out << jsonString(virtualNode->substr(open + 2,
                                              virtualNode->size() - open - 3));
      else
        out << "null";
      out << '}';
    } else {
      out << "null";
    }
    out << '}';
  } else {
    out << "null";
  }
  out << ",\n  \"format\": ";
  if (format)
    out << jsonString(*format);
  else
    out << "null";
  out << ",\n  \"channels\": ";
  writeJsonUnsigned(out, channels);
  out << ",\n  \"channel_positions\": ";
  if (channelPositions) {
    out << '[';
    std::istringstream positions(*channelPositions);
    std::string position;
    bool first = true;
    while (positions >> position) {
      if (!first)
        out << ", ";
      out << jsonString(position);
      first = false;
    }
    out << ']';
  } else {
    out << "null";
  }
  out << ",\n  \"sample_rate_hz\": ";
  writeJsonUnsigned(out, sampleRate);
  out << ",\n  \"quantum_frames\": ";
  writeJsonUnsigned(out, quantum);
  out << ",\n  \"filter_count\": ";
  writeJsonUnsigned(out, filters);
  out << ",\n  \"plugin_reported_latency_sum_samples\": ";
  writeJsonUnsigned(out, pluginLatency);
  out << ",\n  \"plugin_reported_latency_sum_ms\": ";
  if (pluginLatency && sampleRate && *sampleRate)
    writeJsonNumber(out, 1000.0 * static_cast<double>(*pluginLatency) /
                             static_cast<double>(*sampleRate));
  else
    out << "null";
  out << ",\n  \"filter_chain\": ";
  if (filters) {
    out << '[';
    bool first = true;
    size_t begin = status.find("\nFilter chain:");
    if (begin != std::string::npos) {
      begin += std::string("\nFilter chain:").size();
      while (begin < status.size()) {
        const auto end = status.find('\n', begin);
        const auto line = status.substr(begin, end == std::string::npos
                                                   ? std::string::npos
                                                   : end - begin);
        if (line.rfind("Config: ", 0) == 0)
          break;
        const auto firstText = line.find_first_not_of(" \t");
        if (firstText != std::string::npos && line.substr(firstText) != "(none)") {
          if (!first)
            out << ", ";
          out << jsonString(line.substr(firstText));
          first = false;
        }
        if (end == std::string::npos)
          break;
        begin = end + 1;
      }
    }
    out << ']';
  } else {
    out << "null";
  }
  out << ",\n  \"config_path\": ";
  if (config)
    out << jsonString(*config);
  else
    out << "null";
  out << ",\n  \"processed_blocks\": ";
  writeJsonUnsigned(out, blocks);
  out << ",\n  \"overruns\": ";
  writeJsonUnsigned(out, overruns);
  out << ",\n  \"active_capture_links\": ";
  if (activeLinks)
    out << jsonString(*activeLinks);
  else
    out << "null";
  out << ",\n  \"process_average_us\": ";
  writeJsonNumber(out, average);
  out << ",\n  \"process_maximum_us\": ";
  writeJsonNumber(out, maximum);
  out << ",\n  \"input_rms\": ";
  writeJsonNumber(out, inputRms);
  out << ",\n  \"output_rms\": ";
  writeJsonNumber(out, outputRms);
  out << ",\n  \"dsp_amplitude_ratio\": ";
  writeJsonNumber(out, amplitudeRatio);
  out << ",\n  \"plugin_failures\": ";
  if (pluginFailures) {
    out << '[';
    for (size_t i = 0; i < pluginFailures->size(); ++i) {
      if (i)
        out << ", ";
      out << jsonString((*pluginFailures)[i]);
    }
    out << ']';
  } else {
    out << "null";
  }
  out << "\n}\n";
  std::cout << out.str();
}

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
      throw std::runtime_error("usage: skyapo status | diagnostics [--json] | start | stop | restart "
                               "| device list/set/current | config show/reload "
                               "| config check [--json] <file> | plugin list/scan "
                               "| plugin info <URI>");
    std::string cmd = argv[1];
    if (argc == 2 && (cmd == "help" || cmd == "--help" || cmd == "-h")) {
      printUsage(std::cout);
      return 0;
    }
    if (argc == 2 && (cmd == "--version" || cmd == "-V" || cmd == "version")) {
      std::cout << "SkyAPO " << SKYAPO_VERSION << "\nEqualizer APO upstream "
                << SKYAPO_UPSTREAM_REVISION << '\n';
      return 0;
    }
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
    if (cmd == "diagnostics" && argc == 3 &&
        std::string(argv[2]) == "--json") {
      const auto status = settings::queryStatus();
      writeDiagnosticsJson(status);
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
    if (cmd == "plugin" && argc == 6 && std::string(argv[2]) == "set") {
      size_t consumed = 0;
      float value = 0.0f;
      try {
        value = std::stof(argv[5], &consumed);
      } catch (const std::exception &) {
        throw std::runtime_error("plugin parameter value must be numeric");
      }
      if (consumed != std::string(argv[5]).size() || !std::isfinite(value))
        throw std::runtime_error("plugin parameter value must be finite");
      const auto command =
          "PLUGIN_SET " + settings::ipc::lengthPrefixed(argv[3]) + " " +
          settings::ipc::lengthPrefixed(argv[4]) + " " +
          std::to_string(value);
      std::cout << settings::daemonRequest(command);
      return 0;
    }
    if (cmd == "plugin" && argc == 5 &&
        std::string(argv[2]) == "bypass") {
      const std::string state(argv[4]);
      if (state != "on" && state != "off")
        throw std::runtime_error("plugin bypass state must be 'on' or 'off'");
      const auto command =
          "PLUGIN_BYPASS " + settings::ipc::lengthPrefixed(argv[3]) + " " +
          settings::ipc::lengthPrefixed(state);
      std::cout << settings::daemonRequest(command);
      return 0;
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
        std::cout << "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\t"
                     "SAMPLE RATE\tSTABLE PROPERTY\tDEVICE BUS ID\t"
                     "ALSA CARD\tALSA PATH\n";
        for (auto &d : ds.sources)
          if (d.name != "skyapo.virtual_mic")
            std::cout << d.id << '\t' << d.name << '\t' << d.description << '\t'
                      << (d.name == settings::device() ? "yes" : "") << '\t'
                      << (d.channels ? std::to_string(d.channels) : "unknown")
                      << '\t'
                      << (d.sampleRate ? std::to_string(d.sampleRate)
                                       : "unknown")
                      << '\t' << d.identity.stableProperty() << '\t'
                      << d.identity.busId << '\t' << d.identity.alsaCard
                      << '\t' << d.identity.alsaPath << '\n';
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
    if (cmd == "config" && argc == 5 &&
        std::string(argv[2]) == "check" &&
        std::string(argv[3]) == "--json") {
      const std::string file = argv[4];
      std::unique_ptr<Engine> engine;
      try {
        engine = std::make_unique<Engine>(48000, 2, 8192,
                                          std::vector<std::wstring>{}, false,
                                          true, configDeviceMatchText());
        engine->loadConfig(file);
      } catch (const std::exception &error) {
        auto diagnostic = parseConfigDiagnostic(error.what(), file);
        if (const auto *configError =
                dynamic_cast<const Engine::ConfigError *>(&error))
          diagnostic.includeChain = configError->includeSites();
        writeConfigCheckJson(file, nullptr, diagnostic);
        return 1;
      }
      writeConfigCheckJson(file, engine.get(), std::nullopt);
      return 0;
    }
    if (cmd == "config" && argc == 4 && std::string(argv[2]) == "check") {
      Engine e(48000, 2, 8192, {}, false, true, configDeviceMatchText());
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
