#pragma once
#include "IFilter.h"
#include "IFilterFactory.h"
#include "helpers/MemoryHelper.h"
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class FilterConfiguration;
namespace mup {
class ParserX;
}

class Engine {
public:
  struct IncludeSite {
    std::filesystem::path file;
    unsigned line{};
  };
  class ConfigError : public std::runtime_error {
  public:
    ConfigError(const std::string &message, std::vector<IncludeSite> chain)
        : std::runtime_error(message), includeChain(std::move(chain)) {}
    const std::vector<IncludeSite> &includeSites() const noexcept {
      return includeChain;
    }

  private:
    std::vector<IncludeSite> includeChain;
  };

  Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
         std::vector<std::wstring> channelNames = {},
         bool allowPendingEndpointVolume = false,
         bool enablePluginFilters = true);
  void loadConfig(const std::string &path);
  void process(float *interleaved, unsigned frames);
  // Run both upstream configurations on the same unmodified input and apply
  // Equalizer APO's native cosine graph transition. The valid path allocates
  // no memory; callers must prevalidate matching formats and buffer bounds.
  unsigned processTransitionTo(Engine &next, float *interleaved,
                               unsigned frames, unsigned transitionCounter,
                               unsigned transitionLength);
  unsigned filterCount() const { return graph.size(); }
  unsigned sampleRate() const { return rate; }
  unsigned channels() const { return channelCount; }
  unsigned maxFrames() const { return maxFrameCount; }
  bool requiresFixedBlock() const { return fixedBlock; }
  const std::vector<std::string> &filterDescriptions() const {
    return descriptions;
  }
  const std::vector<std::filesystem::path> &configFiles() const {
    return loadedConfigFiles;
  }
  std::vector<std::string> failedPluginDescriptions() const;
  std::optional<uint64_t> pluginLatencySamples() const noexcept;
  // Control-thread only: caller must quiesce the audio graph first.
  unsigned savePersistentPluginStates();
  void setPluginParameter(const std::string &pluginId,
                          const std::string &parameter,
                          float value);
  void setPluginBypass(const std::string &pluginId, bool bypassed);

private:
  struct FilterDeleter {
    void operator()(IFilter *filter) const;
  };
  struct ConfigurationDeleter {
    void operator()(FilterConfiguration *configuration) const;
  };
  struct ParsedFilter {
    std::unique_ptr<IFilter, FilterDeleter> filter;
    std::filesystem::path source;
    unsigned line;
    std::string directive;
    std::vector<IncludeSite> includeChain;
  };
  using FilterList = std::vector<ParsedFilter>;
  struct FilterNode {
    IFilter *filter;
    std::vector<unsigned> inputs;
    std::vector<unsigned> outputs;
    bool inPlace;
    bool fixedBlock;
  };
  void parseConfigFile(const std::filesystem::path &path, FilterList &candidate,
                       std::vector<std::filesystem::path> &includeStack,
                       std::vector<std::filesystem::path> &configFiles,
                       std::vector<IncludeSite> &includeChain,
                       bool &stageActive, mup::ParserX *expressionParser);
  std::vector<FilterNode> buildGraph(FilterList &candidate);
  unsigned rate, channelCount, maxFrameCount;
  bool fixedBlock = false;
  bool allowPendingEndpointVolume = false;
  std::vector<std::string> descriptions;
  std::vector<std::filesystem::path> loadedConfigFiles;
  std::vector<std::unique_ptr<IFilterFactory>> factories;
  std::vector<std::wstring> channelNames;
  std::vector<FilterNode> graph;
  std::unique_ptr<FilterConfiguration, ConfigurationDeleter> configuration;
};
