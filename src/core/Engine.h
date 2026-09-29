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
  unsigned filterCount() const { return filters.size(); }
  unsigned sampleRate() const { return rate; }
  unsigned channels() const { return channelCount; }

private:
  struct FilterDeleter {
    void operator()(IFilter *filter) const;
  };
  using FilterList = std::vector<std::unique_ptr<IFilter, FilterDeleter>>;
  void parseConfigFile(const std::filesystem::path &path, FilterList &candidate,
                       std::vector<std::filesystem::path> &includeStack);
  unsigned rate, channelCount, maxFrames;
  std::vector<std::wstring> channelNames;
  FilterList filters;
  std::vector<std::vector<float>> planar;
  std::vector<std::vector<float>> scratch;
  std::vector<float *> channelPtrs;
  std::vector<float *> scratchPtrs;
};
