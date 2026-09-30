#pragma once

#include "IPluginIdentity.h"
#include <string>

// Optional control-thread interface for changing a plugin's parameters while
// audio is running. Implementations may reject unknown, read-only, or
// out-of-range values by throwing. Calls must be made off the audio thread and
// serialized through one control-thread writer unless an implementation says
// otherwise; the audio thread consumes published values lock-free.
class IPluginParameterControl : public IPluginIdentity {
public:
  virtual ~IPluginParameterControl() = default;
  virtual void setParameterValue(const std::string &symbol, float value) = 0;
};
