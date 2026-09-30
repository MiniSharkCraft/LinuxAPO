#pragma once

// A control-thread-only operation. Callers must first guarantee that no audio
// callback can concurrently enter this plugin instance.
class IPluginStatePersistence {
public:
  virtual ~IPluginStatePersistence() = default;
  virtual bool savePersistentPluginState() = 0;
};
