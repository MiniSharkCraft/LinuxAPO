#pragma once

#include <filesystem>

// Optional parser-to-factory context for plugin data whose identity belongs to
// a particular directive in a configuration file. Kept outside the upstream
// Equalizer APO IFilterFactory ABI.
class IPluginSourceContext {
public:
  virtual ~IPluginSourceContext() = default;
  virtual void setPluginSourceLocation(const std::filesystem::path &source,
                                       unsigned line) = 0;
};
