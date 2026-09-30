#pragma once

#include <cstdint>

// Initial plugin-reported latency snapshot, queried only while building a
// graph on the control thread. It is not end-to-end latency and does not imply
// that the host compensates for it.
class IPluginLatencyState {
public:
  virtual ~IPluginLatencyState() = default;
  virtual uint32_t latencySamples() const noexcept = 0;
};
