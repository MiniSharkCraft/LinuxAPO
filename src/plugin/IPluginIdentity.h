#pragma once

#include <string>

// Stable per-chain identity used by control-plane operations that do not
// depend on a plugin exposing writable parameters.
class IPluginIdentity {
public:
  virtual ~IPluginIdentity() = default;
  virtual const std::string &pluginIdentifier() const noexcept = 0;
};
