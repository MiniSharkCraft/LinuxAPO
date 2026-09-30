#pragma once

#include "IFilterFactory.h"
#include "IPluginInstance.h"
#include <memory>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class CLAPPluginHost final : public IPluginHost {
public:
  CLAPPluginHost();
  ~CLAPPluginHost() override;
  CLAPPluginHost(const CLAPPluginHost &) = delete;
  CLAPPluginHost &operator=(const CLAPPluginHost &) = delete;

  std::unique_ptr<IPluginInstance>
  create(const std::string &id, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) override;
  std::unique_ptr<IPluginInstance>
  createForConfig(const std::string &id, float sampleRate, unsigned maxFrames,
                  const std::vector<std::wstring> &channels,
                  const std::vector<PluginParameterValue> &parameters,
                  const std::filesystem::path &source, unsigned line);
  PluginDescription describe(const std::string &id) const;
  std::vector<std::pair<std::string, std::string>> list() const;

private:
  struct CatalogItem;
  void scan() const;
  mutable bool scanned = false;
  mutable std::vector<CatalogItem> catalog;
};

std::unique_ptr<IFilterFactory> makeCLAPPluginFilterFactory();
