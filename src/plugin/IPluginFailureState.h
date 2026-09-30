#pragma once

#include <string>

// Optional control-thread diagnostics for a plugin filter. Implementations
// latch state from the realtime process call and expose it without logging or
// touching plugin lifecycle from that callback.
class IPluginFailureState {
public:
  virtual ~IPluginFailureState() = default;
  virtual bool processingFailed() const noexcept = 0;
  virtual const std::string &failureIdentifier() const noexcept = 0;
};
