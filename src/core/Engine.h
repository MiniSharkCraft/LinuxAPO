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
    ConfigError(const std::string &message, std::vector<IncludeSite> chain,
                std::vector<std::filesystem::path> attempted = {})
        : std::runtime_error(message), includeChain(std::move(chain)),
          attemptedConfigFiles(std::move(attempted)) {}
    const std::vector<IncludeSite> &includeSites() const noexcept {
      return includeChain;
    }
    const std::vector<std::filesystem::path> &attemptedFiles() const noexcept {
      return attemptedConfigFiles;
    }

  private:
    std::vector<IncludeSite> includeChain;
    std::vector<std::filesystem::path> attemptedConfigFiles;
  };

  Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
         std::vector<std::wstring> channelNames = {},
         bool allowPendingEndpointVolume = false,
         bool enablePluginFilters = true, std::wstring deviceMatchText = {});
  void loadConfig(const std::string &path);
  void process(float *interleaved, unsigned frames);
  // Run both upstream configurations on the same unmodified input and apply
  // Equalizer APO's native cosine graph transition. The valid path allocates
  // no memory; callers must prevalidate matching formats and buffer bounds.
  unsigned processTransitionTo(Engine &next, float *interleaved,
                               unsigned frames, unsigned transitionCounter,
                               unsigned transitionLength);
  unsigned filterCount() const {
    return descriptions.size();
  }
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
  bool pluginLatencyCompensationActive() const noexcept;
  bool pluginLatencyRefreshPending() const noexcept;
  // Control-thread only; realtime callbacks must be quiesced and drained.
  unsigned refreshPluginLatencies();
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
    std::string description;
    std::vector<unsigned> inputs;
    std::vector<unsigned> outputs;
    // Internal channel-lane alignment inserted before each plugin stage.
    bool pdcAlignment{};
    std::vector<unsigned> alignmentChannels;
    // Fixed latency contributed by an upstream filter such as EAPO Delay.
    uint32_t fixedLatencySamples{};
    // Float samples allocated by fixed-latency filters; shares the graph PDC
    // ring budget so Delay and compensation cannot each consume 16 MiB.
    uint64_t fixedBufferSamples{};
    bool inPlace;
    bool fixedBlock;
  };
  void parseConfigFile(const std::filesystem::path &path, FilterList &candidate,
                       std::vector<std::filesystem::path> &includeStack,
                       std::vector<std::filesystem::path> &configFiles,
                       std::vector<std::filesystem::path> &attemptedFiles,
                       std::vector<IncludeSite> &includeChain,
                       bool &stageActive, mup::ParserX *expressionParser);
  std::vector<FilterNode>
  buildGraph(FilterList &candidate,
             std::vector<std::unique_ptr<IFilter, FilterDeleter>> &generated);
  void rebuildPdcPlan(std::vector<FilterNode> &nodes, unsigned laneCount) const;
  unsigned rate, channelCount, maxFrameCount;
  bool fixedBlock = false;
  bool allowPendingEndpointVolume = false;
  std::wstring deviceMatchText;
  std::vector<std::string> descriptions;
  std::vector<std::filesystem::path> loadedConfigFiles;
  std::vector<std::unique_ptr<IFilterFactory>> factories;
  std::vector<std::wstring> channelNames;
  std::vector<FilterNode> graph;
  unsigned allChannelCount{};
  std::unique_ptr<FilterConfiguration, ConfigurationDeleter> configuration;
};
