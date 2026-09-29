#pragma once
#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

private:
  struct FilterDeleter {
    void operator()(IFilter *filter) const;
  };
  struct ParsedFilter {
    std::unique_ptr<IFilter, FilterDeleter> filter;
    std::filesystem::path source;
    unsigned line;
  };
  using FilterList = std::vector<ParsedFilter>;
  struct FilterNode {
    std::unique_ptr<IFilter, FilterDeleter> filter;
    std::vector<unsigned> inputs;
    std::vector<unsigned> outputs;
    bool inPlace;
    bool fixedBlock;
  };
  void parseConfigFile(const std::filesystem::path &path, FilterList &candidate,
                       std::vector<std::filesystem::path> &includeStack);
  std::vector<FilterNode>
  buildGraph(FilterList &candidate, std::vector<std::vector<float>> &newBus,
             std::vector<std::vector<float>> &newScratch,
             std::vector<float *> &newInputs, std::vector<float *> &newOutputs);
  unsigned rate, channelCount, maxFrameCount;
  bool fixedBlock = false;
  std::vector<std::wstring> channelNames;
  std::vector<FilterNode> graph;
  std::vector<std::vector<float>> bus;
  std::vector<std::vector<float>> scratch;
  std::vector<float *> inputPtrs;
  std::vector<float *> outputPtrs;
};
