#pragma once
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("status socket failed");
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  auto p = socketPath();
  if (p.size() >= sizeof(a.sun_path)) {
    close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::copy(p.begin(), p.end(), a.sun_path);
  if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
    close(fd);
    return "Daemon: not reachable\n";
  }
  timeval t{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
  std::string out;
  char b[4096];
  ssize_t n;
  while ((n = read(fd, b, sizeof(b))) > 0)
    out.append(b, n);
  close(fd);
  return out;
}
} // namespace settings
