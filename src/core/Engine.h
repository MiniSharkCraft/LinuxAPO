#pragma once
#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include <memory>
#include <string>
#include <vector>

class Engine {
public:
    Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames, std::vector<std::wstring> channelNames = {});
    void loadConfig(const std::string& path);
    void process(float* interleaved, unsigned frames);
    unsigned filterCount() const { return filters.size(); }
    unsigned sampleRate() const { return rate; }
    unsigned channels() const { return channelCount; }
private:
    struct FilterDeleter { void operator()(IFilter* filter) const; };
    unsigned rate, channelCount, maxFrames;
    std::vector<std::wstring> channelNames;
    std::vector<std::unique_ptr<IFilter, FilterDeleter>> filters;
    std::vector<std::vector<float>> planar;
    std::vector<std::vector<float>> scratch;
    std::vector<float*> channelPtrs;
    std::vector<float*> scratchPtrs;
};
