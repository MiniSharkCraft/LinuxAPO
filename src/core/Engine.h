#pragma once
#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class FilterConfiguration;

class Engine {
public:
  Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
         std::vector<std::wstring> channelNames = {});
  void loadConfig(const std::string &path);
  void process(float *interleaved, unsigned frames);
  unsigned filterCount() const { return graph.size(); }
  unsigned sampleRate() const { return rate; }
  unsigned channels() const { return channelCount; }
  unsigned maxFrames() const { return maxFrameCount; }
  bool requiresFixedBlock() const { return fixedBlock; }
  const std::vector<std::string> &filterDescriptions() const {
    return descriptions;
  }

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
                       bool &stageActive);
  std::vector<FilterNode> buildGraph(FilterList &candidate);
  unsigned rate, channelCount, maxFrameCount;
  bool fixedBlock = false;
  std::vector<std::string> descriptions;
  std::vector<std::wstring> channelNames;
  std::vector<FilterNode> graph;
  std::unique_ptr<FilterConfiguration, ConfigurationDeleter> configuration;
};
