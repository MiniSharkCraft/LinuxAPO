#include "Engine.h"

#include "BiQuadFilterFactory.h"
#include "ChannelFilterFactory.h"
#include "CopyFilter.h"
#include "CopyFilterFactory.h"
#include "DelayFilterFactory.h"
#include "IFilterFactory.h"
#include "IIRFilterFactory.h"
#include "PreampFilterFactory.h"
#include "helpers/ChannelHelper.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

void Engine::FilterDeleter::operator()(IFilter *filter) const {
  if (!filter)
    return;
  filter->~IFilter();
  MemoryHelper::free(filter);
}

Engine::Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
               std::vector<std::wstring> names)
    : rate(sampleRate), channelCount(channels), maxFrames(maxFrames) {
  if (!channels || !maxFrames)
    throw std::runtime_error(
        "sample channels and maximum frame count must be nonzero");
  if (!names.empty() && names.size() != channels)
    throw std::runtime_error("channel names/count mismatch");
  channelNames = std::move(names);
  if (channelNames.empty()) {
    static const std::wstring defaults[] = {L"L",  L"R",  L"C",  L"LFE",
                                            L"RL", L"RR", L"SL", L"SR"};
    if (channels == 1)
      channelNames = {L"C"};
    else if (channels == 2)
      channelNames = {L"L", L"R"};
    else
      for (unsigned c = 0; c < channels; ++c)
        channelNames.push_back(c < 8 ? defaults[c] : std::to_wstring(c + 1));
  }
}

void Engine::loadConfig(const std::string &path) {
  FilterList candidate;
  std::vector<std::filesystem::path> includeStack;
  parseConfigFile(std::filesystem::path(path), candidate, includeStack);

  std::vector<std::vector<float>> newBus, newScratch;
  std::vector<float *> newInputs, newOutputs;
  auto newGraph =
      buildGraph(candidate, newBus, newScratch, newInputs, newOutputs);
  graph.swap(newGraph);
  bus.swap(newBus);
  scratch.swap(newScratch);
  inputPtrs.swap(newInputs);
  outputPtrs.swap(newOutputs);
}

std::vector<Engine::FilterNode> Engine::buildGraph(
    FilterList &candidate, std::vector<std::vector<float>> &newBus,
    std::vector<std::vector<float>> &newScratch,
    std::vector<float *> &newInputs, std::vector<float *> &newOutputs) {
  std::vector<std::wstring> allNames = channelNames;
  std::vector<std::wstring> selectedNames = allNames;
  std::vector<FilterNode> result;
  result.reserve(candidate.size());

  for (auto &parsed : candidate) {
    IFilter *filter = parsed.filter.get();
    const auto savedSelection = selectedNames;
    if (filter->getAllChannels())
      selectedNames = allNames;
    const auto inputNames = selectedNames;
    if (auto *copy = dynamic_cast<CopyFilter *>(filter))
      for (const auto &assignment : copy->getAssignments())
        for (const auto &summand : assignment.sourceSum)
          if (!summand.channel.empty() &&
              ChannelHelper::getChannelIndex(summand.channel, inputNames) < 0)
            throw std::runtime_error(
                parsed.source.string() + ":" + std::to_string(parsed.line) +
                ": Copy references unknown source channel '" +
                StringHelper::toString(summand.channel, 65001) + "'");
    auto outputNames =
        filter->initialize(static_cast<float>(rate), maxFrames, inputNames);
    if (outputNames.empty())
      throw std::runtime_error(parsed.source.string() + ":" +
                               std::to_string(parsed.line) +
                               ": directive selected or produced no channels");

    FilterNode node;
    node.inPlace = filter->getInPlace();
    for (const auto &name : inputNames) {
      auto it = std::find(allNames.begin(), allNames.end(), name);
      if (it == allNames.end())
        throw std::runtime_error(parsed.source.string() + ":" +
                                 std::to_string(parsed.line) +
                                 ": input channel is not available: " +
                                 StringHelper::toString(name, 65001));
      node.inputs.push_back(static_cast<unsigned>(it - allNames.begin()));
    }
    for (const auto &name : outputNames) {
      auto it = std::find(allNames.begin(), allNames.end(), name);
      if (it == allNames.end()) {
        if (std::find(channelNames.begin(), channelNames.end(), name) ==
            channelNames.end())
          throw std::runtime_error(
              parsed.source.string() + ":" + std::to_string(parsed.line) +
              ": Copy creates channel '" + StringHelper::toString(name, 65001) +
              "' beyond the fixed PipeWire output layout");
        node.outputs.push_back(static_cast<unsigned>(allNames.size()));
        allNames.push_back(name);
      } else {
        node.outputs.push_back(static_cast<unsigned>(it - allNames.begin()));
      }
    }
    node.filter = std::move(parsed.filter);
    result.push_back(std::move(node));
    selectedNames =
        filter->getSelectChannels() ? std::move(outputNames) : savedSelection;
  }

  newBus.resize(allNames.size(), std::vector<float>(maxFrames));
  newScratch.resize(allNames.size(), std::vector<float>(maxFrames));
  newInputs.resize(allNames.size());
  newOutputs.resize(allNames.size());
  return result;
}

void Engine::parseConfigFile(const std::filesystem::path &configPath,
                             FilterList &candidate,
                             std::vector<std::filesystem::path> &includeStack) {
  std::error_code ec;
  auto absolutePath = std::filesystem::absolute(configPath, ec);
  if (ec)
    throw std::runtime_error("cannot resolve config path '" +
                             configPath.string() + "': " + ec.message());
  auto normalizedPath = std::filesystem::weakly_canonical(absolutePath, ec);
  if (ec)
    normalizedPath = absolutePath.lexically_normal();
  if (includeStack.size() >= 100)
    throw std::runtime_error("include nesting exceeds 100 files at " +
                             normalizedPath.string());
  if (std::find(includeStack.begin(), includeStack.end(), normalizedPath) !=
      includeStack.end())
    throw std::runtime_error("include cycle detected at " +
                             normalizedPath.string());

  std::ifstream in(normalizedPath);
  if (!in)
    throw std::runtime_error("cannot open config: " + normalizedPath.string());
  includeStack.push_back(normalizedPath);
  struct PopPath {
    std::vector<std::filesystem::path> &stack;
    ~PopPath() { stack.pop_back(); }
  } popPath{includeStack};

  PreampFilterFactory preamp;
  BiQuadFilterFactory biquad;
  IIRFilterFactory iir;
  DelayFilterFactory delay;
  ChannelFilterFactory channel;
  CopyFilterFactory copy;
  std::string raw;
  unsigned lineNo = 0;
  while (std::getline(in, raw)) {
    ++lineNo;
    auto comment = raw.find('#');
    if (comment != std::string::npos)
      raw.resize(comment);
    auto line = StringHelper::trim(StringHelper::toWString(raw, 65001));
    if (line.empty())
      continue;
    const auto colon = line.find(L':');
    if (colon == line.npos)
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": expected command:");
    std::wstring command = StringHelper::trim(line.substr(0, colon));
    std::wstring params = StringHelper::trim(line.substr(colon + 1));
    const auto widePath =
        StringHelper::toWString(normalizedPath.string(), 65001);

    if (command == L"Include") {
      if (params.size() >= 2 && params.front() == L'"' && params.back() == L'"')
        params = params.substr(1, params.size() - 2);
      if (params.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Include requires a path");
      std::filesystem::path included(StringHelper::toString(params, 65001));
      if (included.is_relative())
        included = normalizedPath.parent_path() / included;
      parseConfigFile(included, candidate, includeStack);
      continue;
    }

    std::vector<IFilter *> made;
    if (command == L"Preamp")
      made = preamp.createFilter(widePath, command, params);
    else if (command == L"Filter") {
      made = biquad.createFilter(widePath, command, params);
      if (made.empty())
        made = iir.createFilter(widePath, command, params);
    } else if (command == L"Delay")
      made = delay.createFilter(widePath, command, params);
    else if (command == L"Channel")
      made = channel.createFilter(widePath, command, params);
    else if (command == L"Copy")
      made = copy.createFilter(widePath, command, params);
    else
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) +
                               ": unsupported command '" +
                               StringHelper::toString(command, 65001) + "'");
    if (made.empty())
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": invalid " +
                               StringHelper::toString(command, 65001) +
                               " parameters");
    for (auto *filter : made)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, lineNo});
  }
  if (in.bad())
    throw std::runtime_error("error reading config: " +
                             normalizedPath.string());
}

void Engine::process(float *samples, unsigned frames) {
  if (frames > maxFrames)
    throw std::runtime_error("frame block exceeds configured maximum");
  for (unsigned c = 0; c < channelCount; ++c)
    for (unsigned f = 0; f < frames; ++f)
      bus[c][f] = samples[f * channelCount + c];
  for (auto &node : graph) {
    for (size_t i = 0; i < node.inputs.size(); ++i)
      inputPtrs[i] = bus[node.inputs[i]].data();
    for (size_t i = 0; i < node.outputs.size(); ++i)
      outputPtrs[i] =
          node.inPlace ? bus[node.outputs[i]].data() : scratch[i].data();
    node.filter->process(outputPtrs.data(), inputPtrs.data(), frames);
    if (!node.inPlace)
      for (size_t i = 0; i < node.outputs.size(); ++i)
        std::copy_n(scratch[i].data(), frames, bus[node.outputs[i]].data());
  }
  for (unsigned c = 0; c < channelCount; ++c)
    for (unsigned f = 0; f < frames; ++f)
      samples[f * channelCount + c] = bus[c][f];
}
