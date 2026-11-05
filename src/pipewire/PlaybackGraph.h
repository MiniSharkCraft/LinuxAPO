#pragma once

#include "DeviceManager.h"

#include <memory>
#include <string>

struct pw_core;
struct pw_loop;
struct pw_registry;
struct ConfigSourceSnapshot;

namespace skyapo::pipewire {

class PlaybackGraph {
public:
  PlaybackGraph(pw_core *core, pw_loop *loop, pw_registry *registry,
                AudioDevice device, std::vector<AudioPort> ports,
                std::string configPath);
  ~PlaybackGraph();

  PlaybackGraph(const PlaybackGraph &) = delete;
  PlaybackGraph &operator=(const PlaybackGraph &) = delete;

  void reloadConfig(const std::string &configPath);
  void reloadConfig(const std::string &configPath,
                    std::shared_ptr<const ConfigSourceSnapshot> sourceSnapshot);
  void selectSink(AudioDevice device, std::vector<AudioPort> ports);
  std::string status() const;
  const AudioDevice &device() const noexcept;
  bool ready() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skyapo::pipewire
