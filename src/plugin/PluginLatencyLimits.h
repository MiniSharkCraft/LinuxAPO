#pragma once

#include <cstdint>

namespace skyapo::plugin {

// The supported formats are used in a low-latency microphone path. Cap both
// reported plugin latency and host compensation rings so malformed metadata
// cannot request unbounded memory. This is 10000 samples (about 104-227 ms at
// the currently supported 44.1/48/96 kHz rates).
inline constexpr uint32_t MaxRealtimeLatencySamples = 10000;

} // namespace skyapo::plugin
