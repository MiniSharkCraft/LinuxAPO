#pragma once
#include <charconv>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace settings {
class DaemonLock {
  int fd = -1;

public:
  DaemonLock() {
    auto path = socketPath();
    fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) {
      if (fd >= 0)
        close(fd);
      fd = -1;
      throw std::runtime_error(
          "another SkyAPO daemon is running or runtime lock unavailable");
    }
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        fd = -1;
        throw std::runtime_error("unexpected object at runtime socket path");
      }
      if (unlink(path.c_str()) < 0) {
        close(fd);
        fd = -1;
        throw std::runtime_error("cannot remove stale runtime socket");
      }
    }
  }
  ~DaemonLock() {
    if (fd >= 0)
      close(fd);
  }
  DaemonLock(const DaemonLock &) = delete;
  DaemonLock &operator=(const DaemonLock &) = delete;

private:
  static std::string socketPath() {
    auto *x = getenv("XDG_RUNTIME_DIR");
    if (!x)
      throw std::runtime_error("XDG_RUNTIME_DIR is unset");
    return std::string(x) + "/skyapo.sock";
  }
};
inline std::filesystem::path configDir() {
  auto *x = getenv("XDG_CONFIG_HOME");
  if (x && *x)
    return std::filesystem::path(x) / "skyapo";
  auto *h = getenv("HOME");
  if (!h)
    throw std::runtime_error("HOME is unset");
  return std::filesystem::path(h) / ".config/skyapo";
}
inline std::string socketPath() {
  auto *x = getenv("XDG_RUNTIME_DIR");
  if (!x)
    throw std::runtime_error("XDG_RUNTIME_DIR is unset");
  return std::string(x) + "/skyapo.sock";
}
namespace ipc {
inline constexpr unsigned ProtocolVersion = 1;
inline constexpr size_t MaxRequestBytes = 128;
inline constexpr size_t MaxResponseBytes = 4 * 1024 * 1024;

enum class Command {
  Status,
  Reload,
  Stop,
  SetPluginParameter,
  SetPluginBypass,
  Invalid
};
struct DecodedRequest {
  Command command = Command::Invalid;
  std::string error;
  std::string pluginId;
  std::string parameter;
  float value{};
  bool bypassed{};
};
struct DecodedResponse {
  bool ok = false;
  std::string payload;
};

inline std::string requestFrame(const std::string &command) {
  if (command != "STATUS" && command != "RELOAD" && command != "STOP" &&
      command.rfind("PLUGIN_SET ", 0) != 0 &&
      command.rfind("PLUGIN_BYPASS ", 0) != 0)
    throw std::runtime_error("unsupported daemon command");
  const std::string frame = "SKYAPO/" + std::to_string(ProtocolVersion) +
                            " " + command + "\n";
  if (frame.size() > MaxRequestBytes)
    throw std::runtime_error("daemon IPC request exceeds size limit");
  return frame;
}

inline bool readLengthPrefixed(const std::string &text, size_t &offset,
                               std::string &value) {
  const size_t colon = text.find(':', offset);
  if (colon == std::string::npos || colon == offset)
    return false;
  size_t length = 0;
  const auto parsed = std::from_chars(text.data() + offset,
                                      text.data() + colon, length);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + colon ||
      length > text.size() - colon - 1)
    return false;
  offset = colon + 1;
  value.assign(text.data() + offset, length);
  offset += length;
  return true;
}

inline std::string lengthPrefixed(const std::string &value) {
  return std::to_string(value.size()) + ":" + value;
}

inline DecodedRequest decodeRequest(const std::string &frame) {
  if (frame.size() > MaxRequestBytes)
    return {Command::Invalid, "request frame exceeds size limit"};
  if (frame.rfind("SKYAPO/", 0) != 0) {
    if (frame == "STATUS\n" || frame == "RELOAD\n" || frame == "STOP\n")
      return {Command::Invalid,
              "unsupported protocol version (legacy request)"};
    return {Command::Invalid, "malformed request frame"};
  }
  const auto separator = frame.find(' ');
  const auto version = "SKYAPO/" + std::to_string(ProtocolVersion);
  if (separator == std::string::npos ||
      frame.substr(0, separator) != version)
    return {Command::Invalid, "unsupported protocol version"};
  if (frame.size() <= separator + 1 || frame.back() != '\n' ||
      frame.find('\n') != frame.size() - 1)
    return {Command::Invalid, "malformed request frame"};
  const auto command = frame.substr(separator + 1,
                                    frame.size() - separator - 2);
  if (command == "STATUS")
    return {Command::Status, {}};
  if (command == "RELOAD")
    return {Command::Reload, {}};
  if (command == "STOP")
    return {Command::Stop, {}};
  constexpr std::string_view pluginPrefix = "PLUGIN_SET ";
  if (command.rfind(pluginPrefix.data(), 0) == 0) {
    const auto payload = command.substr(pluginPrefix.size());
    size_t offset = 0;
    std::string pluginId, parameter;
    if (!readLengthPrefixed(payload, offset, pluginId) || pluginId.empty() ||
        offset >= payload.size() || payload[offset++] != ' ' ||
        !readLengthPrefixed(payload, offset, parameter) || parameter.empty() ||
        offset >= payload.size() || payload[offset++] != ' ')
      return {Command::Invalid, "malformed PLUGIN_SET request"};
    const char *valueBegin = payload.data() + offset;
    char *valueEnd = nullptr;
    errno = 0;
    const float value = std::strtof(valueBegin, &valueEnd);
    if (errno == ERANGE || valueEnd == valueBegin ||
        valueEnd != payload.data() + payload.size() || !std::isfinite(value))
      return {Command::Invalid, "invalid PLUGIN_SET parameter value"};
    DecodedRequest request{Command::SetPluginParameter, {}};
    request.pluginId = std::move(pluginId);
    request.parameter = std::move(parameter);
    request.value = value;
    return request;
  }
  constexpr std::string_view bypassPrefix = "PLUGIN_BYPASS ";
  if (command.rfind(bypassPrefix.data(), 0) == 0) {
    const auto payload = command.substr(bypassPrefix.size());
    size_t offset = 0;
    std::string pluginId, value;
    if (!readLengthPrefixed(payload, offset, pluginId) || pluginId.empty() ||
        offset >= payload.size() || payload[offset++] != ' ' ||
        !readLengthPrefixed(payload, offset, value) ||
        offset != payload.size())
      return {Command::Invalid, "malformed PLUGIN_BYPASS request"};
    if (value != "on" && value != "off")
      return {Command::Invalid, "PLUGIN_BYPASS state must be on or off"};
    DecodedRequest request{Command::SetPluginBypass, {}};
    request.pluginId = std::move(pluginId);
    request.bypassed = value == "on";
    return request;
  }
  return {Command::Invalid, "unsupported daemon command"};
}

inline std::string responseFrame(bool ok, const std::string &payload) {
  const bool payloadFits = payload.size() <= MaxResponseBytes;
  const std::string body =
      payloadFits ? payload : "daemon IPC response exceeds size limit\n";
  return "SKYAPO/" + std::to_string(ProtocolVersion) + " " +
         std::string(ok && payloadFits ? "OK " : "ERR ") +
         std::to_string(body.size()) + "\n" + body;
}

inline DecodedResponse decodeResponse(const std::string &frame) {
  const auto newline = frame.find('\n');
  if (newline == std::string::npos)
    throw std::runtime_error("malformed daemon IPC response header");
  const auto header = frame.substr(0, newline);
  if (header.rfind("SKYAPO/", 0) != 0)
    throw std::runtime_error("unsupported legacy daemon IPC response");
  const auto firstSpace = header.find(' ');
  if (firstSpace == std::string::npos)
    throw std::runtime_error("malformed daemon IPC response header");
  if (header.substr(0, firstSpace) !=
      "SKYAPO/" + std::to_string(ProtocolVersion))
    throw std::runtime_error("unsupported daemon IPC protocol version");
  const auto secondSpace = header.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos)
    throw std::runtime_error("malformed daemon IPC response header");
  const auto kind = header.substr(firstSpace + 1,
                                  secondSpace - firstSpace - 1);
  if (kind != "OK" && kind != "ERR")
    throw std::runtime_error("malformed daemon IPC response status");
  const auto lengthText = header.substr(secondSpace + 1);
  uint64_t length = 0;
  const auto parsed = std::from_chars(lengthText.data(),
                                      lengthText.data() + lengthText.size(),
                                      length);
  if (lengthText.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != lengthText.data() + lengthText.size() ||
      length > MaxResponseBytes)
    throw std::runtime_error("invalid daemon IPC response length");
  const auto payload = frame.substr(newline + 1);
  if (payload.size() != length)
    throw std::runtime_error("daemon IPC response length mismatch");
  return {kind == "OK", payload};
}
} // namespace ipc

inline std::optional<std::string> exchangeDaemonRequest(
    const std::string &command, unsigned timeoutSeconds) {
  const auto request = ipc::requestFrame(command);
  const auto path = socketPath();
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("daemon control socket failed");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::copy(path.begin(), path.end(), address.sun_path);
  timeval timeout{static_cast<time_t>(timeoutSeconds), 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    close(fd);
    return std::nullopt;
  }
  size_t sent = 0;
  while (sent < request.size()) {
    const auto count = send(fd, request.data() + sent, request.size() - sent,
                            MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      close(fd);
      throw std::runtime_error("cannot send daemon IPC request");
    }
    sent += static_cast<size_t>(count);
  }
  shutdown(fd, SHUT_WR);
  std::string frame;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = read(fd, buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      close(fd);
      throw std::runtime_error("cannot read daemon IPC response");
    }
    if (frame.size() + static_cast<size_t>(count) >
        ipc::MaxResponseBytes + 128) {
      close(fd);
      throw std::runtime_error("daemon IPC response exceeds size limit");
    }
    frame.append(buffer.data(), static_cast<size_t>(count));
  }
  close(fd);
  if (frame.empty())
    throw std::runtime_error("daemon returned an empty response");
  auto response = ipc::decodeResponse(frame);
  if (!response.ok)
    throw std::runtime_error(response.payload.empty()
                                 ? "daemon rejected IPC request"
                                 : response.payload);
  return response.payload;
}
inline std::string device() {
  std::ifstream f(configDir() / "device");
  std::string s;
  std::getline(f, s);
  return s;
}
inline void select(const std::string &name) {
  std::filesystem::create_directories(configDir());
  auto path = configDir() / "device";
  auto temp = path.string() + ".tmp." + std::to_string(getpid());
  {
    std::ofstream f(temp);
    f << name << '\n';
    if (!f)
      throw std::runtime_error("cannot save device");
  }
  std::filesystem::rename(temp, path);
}
inline std::string config() {
  auto dir = configDir();
  std::filesystem::create_directories(dir);
  auto p = dir / "config.txt";
  if (!std::filesystem::exists(p)) {
    std::ofstream f(p);
    f << "Preamp: 0 dB\n";
    if (!f)
      throw std::runtime_error("cannot create config");
  }
  return p.string();
}
inline std::string queryStatus() {
  auto response = exchangeDaemonRequest("STATUS", 2);
  if (!response)
    return "Daemon: not reachable\n";
  return *response;
}
inline std::string daemonRequest(const std::string &request) {
  auto command = request;
  while (!command.empty() &&
         (command.back() == '\n' || command.back() == '\r'))
    command.pop_back();
  auto response = exchangeDaemonRequest(command, 3);
  if (!response)
    throw std::runtime_error("daemon is not reachable");
  return *response;
}
} // namespace settings
