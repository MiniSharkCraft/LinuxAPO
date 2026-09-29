#include "Engine.h"
#include "FilterConfiguration.h"
#include "FilterEngine.h"

#include "BiQuadFilterFactory.h"
#include "ChannelFilterFactory.h"
#include "CopyFilter.h"
#include "CopyFilterFactory.h"
#include "DelayFilterFactory.h"
#ifdef SKYAPO_HAVE_CONVOLUTION
#include "ConvolutionFilter.h"
#include "ConvolutionFilterFactory.h"
#include "GraphicEQFilter.h"
#include "GraphicEQFilterFactory.h"
#endif
#include "IFilterFactory.h"
#include "IIRFilterFactory.h"
#include "PreampFilterFactory.h"
#ifdef SKYAPO_HAVE_LV2
#include "LV2PluginHost.h"
#endif
#include "helpers/ChannelHelper.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

void Engine::FilterDeleter::operator()(IFilter *filter) const {
  if (!filter)
    return;
  filter->~IFilter();
  MemoryHelper::free(filter);
}

void Engine::ConfigurationDeleter::operator()(
    FilterConfiguration *configuration) const {
  if (!configuration)
    return;
  configuration->~FilterConfiguration();
  MemoryHelper::free(configuration);
}

Engine::Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
               std::vector<std::wstring> names)
    : rate(sampleRate), channelCount(channels), maxFrameCount(maxFrames) {
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
  factories.push_back(std::make_unique<ChannelFilterFactory>());
  factories.push_back(std::make_unique<IIRFilterFactory>());
  factories.push_back(std::make_unique<BiQuadFilterFactory>());
  factories.push_back(std::make_unique<PreampFilterFactory>());
  factories.push_back(std::make_unique<DelayFilterFactory>());
  factories.push_back(std::make_unique<CopyFilterFactory>());
#ifdef SKYAPO_HAVE_LV2
  factories.push_back(makeLV2PluginFilterFactory());
#endif
#ifdef SKYAPO_HAVE_CONVOLUTION
  factories.push_back(std::make_unique<ConvolutionFilterFactory>());
  factories.push_back(std::make_unique<GraphicEQFilterFactory>());
#endif
}

void Engine::loadConfig(const std::string &path) {
  FilterList candidate;
  std::vector<std::filesystem::path> includeStack;
  bool stageActive = true;
  FilterEngine factoryContext(channelCount, channelCount, maxFrameCount);
  const auto addReturnedFilters =
      [&](std::vector<IFilter *> produced, const std::filesystem::path &source,
          unsigned line, const std::string &directive) {
        for (auto *filter : produced)
          candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                               source, line, directive});
      };
  for (auto &factory : factories) {
    factory->initialize(&factoryContext);
    addReturnedFilters(factory->startOfConfiguration(), path, 0,
                       "configuration initialization");
  }
  parseConfigFile(std::filesystem::path(path), candidate, includeStack,
                  stageActive);
  for (auto &factory : factories)
    addReturnedFilters(factory->endOfConfiguration(), path, 0,
                       "configuration finalization");

  auto newGraph = buildGraph(candidate);
  const bool newFixedBlock =
      std::any_of(newGraph.begin(), newGraph.end(),
                  [](const auto &node) { return node.fixedBlock; });
  std::vector<std::string> newDescriptions;
  newDescriptions.reserve(candidate.size());
  for (const auto &parsed : candidate)
    newDescriptions.push_back(parsed.directive + " — " +
                              parsed.source.string() + ":" +
                              std::to_string(parsed.line));

  std::vector<FilterInfo *> infos;
  infos.reserve(newGraph.size());
  auto freeInfos = [&infos] {
    for (auto *info : infos) {
      MemoryHelper::free(info->inChannels);
      MemoryHelper::free(info->outChannels);
      MemoryHelper::free(info);
    }
    infos.clear();
  };
  try {
    for (const auto &node : newGraph) {
      auto *info =
          static_cast<FilterInfo *>(MemoryHelper::alloc(sizeof(FilterInfo)));
      info->filter = node.filter;
      info->inPlace = node.inPlace;
      info->inChannelCount = node.inputs.size();
      info->outChannelCount = node.outputs.size();
      info->inChannels = nullptr;
      info->outChannels = nullptr;
      try {
        if (info->inChannelCount) {
          info->inChannels = static_cast<size_t *>(
              MemoryHelper::alloc(info->inChannelCount * sizeof(size_t)));
          std::copy(node.inputs.begin(), node.inputs.end(), info->inChannels);
        }
        if (info->outChannelCount) {
          info->outChannels = static_cast<size_t *>(
              MemoryHelper::alloc(info->outChannelCount * sizeof(size_t)));
          std::copy(node.outputs.begin(), node.outputs.end(),
                    info->outChannels);
        }
      } catch (...) {
        MemoryHelper::free(info->inChannels);
        MemoryHelper::free(info->outChannels);
        MemoryHelper::free(info);
        throw;
      }
      infos.push_back(info);
    }
  } catch (...) {
    freeInfos();
    throw;
  }

  FilterEngine context(channelCount, channelCount, maxFrameCount);
  void *memory = MemoryHelper::alloc(sizeof(FilterConfiguration));
  FilterConfiguration *built = nullptr;
  try {
    built =
        new (memory) FilterConfiguration(&context, infos, channelNames.size());
  } catch (...) {
    MemoryHelper::free(memory);
    freeInfos();
    throw;
  }
  std::unique_ptr<FilterConfiguration, ConfigurationDeleter> newConfiguration(
      built);
  for (auto &parsed : candidate)
    parsed.filter.release();

  graph.swap(newGraph);
  configuration.swap(newConfiguration);
  descriptions.swap(newDescriptions);
  fixedBlock = newFixedBlock;
}

std::vector<Engine::FilterNode> Engine::buildGraph(FilterList &candidate) {
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
    bool usesConvolution = false;
#ifdef SKYAPO_HAVE_CONVOLUTION
    usesConvolution = dynamic_cast<ConvolutionFilter *>(filter) != nullptr;
    if (usesConvolution && maxFrameCount % 4 != 0)
      throw std::runtime_error(parsed.source.string() + ":" +
                               std::to_string(parsed.line) +
                               ": upstream libHybridConv requires a block "
                               "size divisible by four samples");
    if (auto *graphic = dynamic_cast<GraphicEQFilter *>(filter)) {
      if (graphic->getNodes().empty())
        throw std::runtime_error(parsed.source.string() + ":" +
                                 std::to_string(parsed.line) +
                                 ": GraphicEQ requires a frequency/gain pair");
      for (const auto &point : graphic->getNodes())
        if (!std::isfinite(point.freq) || point.freq <= 0 ||
            !std::isfinite(point.dbGain))
          throw std::runtime_error(parsed.source.string() + ":" +
                                   std::to_string(parsed.line) +
                                   ": GraphicEQ frequencies must be positive "
                                   "and values finite");
    }
#endif
    std::vector<std::wstring> outputNames;
    try {
      outputNames = filter->initialize(static_cast<float>(rate), maxFrameCount,
                                       inputNames);
    } catch (const std::exception &e) {
      throw std::runtime_error(parsed.source.string() + ":" +
                               std::to_string(parsed.line) + ": " + e.what());
    }
    if (outputNames.empty())
      throw std::runtime_error(parsed.source.string() + ":" +
                               std::to_string(parsed.line) +
                               ": directive selected or produced no channels");

    FilterNode node;
    node.inPlace = filter->getInPlace();
    node.fixedBlock = usesConvolution;
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
    node.filter = filter;
    result.push_back(std::move(node));
    selectedNames =
        filter->getSelectChannels() ? std::move(outputNames) : savedSelection;
  }

  return result;
}

void Engine::parseConfigFile(const std::filesystem::path &configPath,
                             FilterList &candidate,
                             std::vector<std::filesystem::path> &includeStack,
                             bool &stageActive) {
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

  const auto widePath = StringHelper::toWString(normalizedPath.string(), 65001);
  for (auto &factory : factories) {
    auto produced = factory->startOfFile(widePath);
    for (auto *filter : produced)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, 0, "file initialization"});
  }
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

    if (command == L"Stage") {
      std::wistringstream stages(StringHelper::toLowerCase(params));
      std::wstring stage;
      bool foundCapture = false;
      bool foundAny = false;
      while (stages >> stage) {
        foundAny = true;
        if (stage == L"capture")
          foundCapture = true;
        else if (stage != L"pre-mix" && stage != L"post-mix")
          throw std::runtime_error(normalizedPath.string() + ":" +
                                   std::to_string(lineNo) +
                                   ": unsupported Stage value '" +
                                   StringHelper::toString(stage, 65001) + "'");
      }
      if (!foundAny)
        throw std::runtime_error(
            normalizedPath.string() + ":" + std::to_string(lineNo) +
            ": Stage requires capture, pre-mix, or post-mix");
      stageActive = foundCapture;
      continue;
    }
    if (!stageActive)
      continue;

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
      bool includedStage = stageActive;
      parseConfigFile(included, candidate, includeStack, includedStage);
      continue;
    }

    const auto originalCommand = command;
    std::vector<IFilter *> made;
    for (auto &factory : factories) {
      made = factory->createFilter(widePath, command, params);
      if (!made.empty() || command.empty())
        break;
    }
    if (command.empty())
      continue;
    if (made.empty()) {
      const auto name = StringHelper::toString(originalCommand, 65001);
      const bool known =
          originalCommand == L"Preamp" || originalCommand == L"Delay" ||
          originalCommand == L"Channel" || originalCommand == L"Copy" ||
          originalCommand.rfind(L"Filter", 0) == 0 ||
          originalCommand == L"GraphicEQ" ||
          originalCommand == L"Convolution" || originalCommand == L"Plugin";
      if (!known)
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": unsupported command '" + name + "'");
#ifndef SKYAPO_HAVE_CONVOLUTION
      if (originalCommand == L"GraphicEQ" || originalCommand == L"Convolution")
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) + ": " + name +
                                 " requires FFTW3f development files");
#endif
#ifndef SKYAPO_HAVE_LV2
      if (originalCommand == L"Plugin")
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) + ": " + name +
                                 " requires Lilv development files for LV2");
#endif
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": invalid " + name +
                               " parameters");
    }
    for (auto *filter : made)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, lineNo,
                           StringHelper::toString(originalCommand, 65001)});
  }
  if (in.bad())
    throw std::runtime_error("error reading config: " +
                             normalizedPath.string());
  for (auto &factory : factories) {
    auto produced = factory->endOfFile(widePath);
    for (auto *filter : produced)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, lineNo, "file finalization"});
  }
}

void Engine::process(float *samples, unsigned frames) {
  if (frames > maxFrameCount)
    throw std::runtime_error("frame block exceeds configured maximum");
  if (fixedBlock && frames != maxFrameCount)
    throw std::runtime_error(
        "convolution filters require the negotiated fixed audio block size");
  configuration->read(samples, frames);
  configuration->process(frames);
  configuration->write(samples, frames);
}
