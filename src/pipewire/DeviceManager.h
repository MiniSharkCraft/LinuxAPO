#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct AudioDevice {
  uint32_t id;
  std::string name, description;
  unsigned channels{}, sampleRate{};
};
struct AudioPort {
  uint32_t id, node;
  std::string direction, channel, name;
};
struct Devices {
  std::vector<AudioDevice> sources;
  std::vector<AudioPort> ports;
};
Devices enumerateDevices();
