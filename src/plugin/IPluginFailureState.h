#pragma once

#include <string>

// Optional diagnostics/failure control for a plugin filter. Implementations
// latch failures from realtime processing or bounded control-thread checks and
// expose them without logging or touching plugin lifecycle in the callback.
class IPluginFailureState {
public:
  virtual ~IPluginFailureState() = default;
  virtual bool processingFailed() const noexcept = 0;
  virtual const std::string &failureIdentifier() const noexcept = 0;
  virtual void latchProcessingFailure() noexcept {}
};
