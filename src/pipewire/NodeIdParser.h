#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace skyapo::pipewire {

// PipeWire registry properties are external metadata. Accept only a complete,
// in-range decimal uint32 so malformed NODE_ID values are ignored rather than
// allowing an exception to escape a C callback.
inline std::optional<uint32_t> parseNodeId(std::string_view text) noexcept {
  if (text.empty())
    return std::nullopt;

  uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                      value, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return std::nullopt;
  return value;
}

} // namespace skyapo::pipewire
