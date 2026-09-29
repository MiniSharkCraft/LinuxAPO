#include "Engine.h"
#include "BiQuadFilterFactory.h"
#include "DelayFilterFactory.h"
#include "IFilterFactory.h"
#include "IIRFilterFactory.h"
#include "PreampFilterFactory.h"
#include "helpers/StringHelper.h"
#include <algorithm>
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

Engine::Engine(unsigned sampleRate, unsigned channels, unsigned maxFrames,
               std::vector<std::wstring> names)
    : rate(sampleRate), channelCount(channels), maxFrames(maxFrames),
      planar(channels, std::vector<float>(maxFrames)),
      scratch(channels, std::vector<float>(maxFrames)), channelPtrs(channels),
      scratchPtrs(channels) {
  if (!names.empty() && names.size() != channels)
    throw std::runtime_error("channel names/count mismatch");
  channelNames = std::move(names);
  for (unsigned c = 0; c < channels; ++c) {
    channelPtrs[c] = planar[c].data();
    scratchPtrs[c] = scratch[c].data();
  }
}

void Engine::loadConfig(const std::string &path) {
  FilterList candidate;
  std::vector<std::filesystem::path> includeStack;
  parseConfigFile(std::filesystem::path(path), candidate, includeStack);
  filters.swap(candidate);
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
    auto colon = line.find(L':');
    if (colon == line.npos)
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": expected command:");
    std::wstring command = StringHelper::trim(line.substr(0, colon));
    std::wstring params = StringHelper::trim(line.substr(colon + 1));
    std::vector<IFilter *> made;
    const auto widePath =
        StringHelper::toWString(normalizedPath.string(), 65001);
    if (command == L"Include") {
      if (params.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Include requires a path");
      std::wstring includeName = params;
      if (includeName.size() >= 2 && includeName.front() == L'"' &&
          includeName.back() == L'"')
        includeName = includeName.substr(1, includeName.size() - 2);
      if (includeName.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Include requires a path");
      auto includeBytes = StringHelper::toString(includeName, 65001);
      std::filesystem::path included(includeBytes);
      if (included.is_relative())
        included = normalizedPath.parent_path() / included;
      parseConfigFile(included, candidate, includeStack);
      continue;
    }
    if (command == L"Preamp")
      made = preamp.createFilter(widePath, command, params);
    else if (command == L"Filter") {
      made = biquad.createFilter(widePath, command, params);
      if (made.empty())
        made = iir.createFilter(widePath, command, params);
    } else if (command == L"Delay")
      made = delay.createFilter(widePath, command, params);
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
    static const std::wstring positions[] = {L"L",  L"R",  L"C",  L"LFE",
                                             L"RL", L"RR", L"SL", L"SR"};
    std::vector<std::wstring> channels;
    if (channelCount == 1)
      channels = {L"C"};
    else if (channelCount == 2)
      channels = {L"L", L"R"};
    else
      for (unsigned c = 0; c < channelCount; ++c)
        channels.push_back(c < 8 ? positions[c] : std::to_wstring(c + 1));
    if (!channelNames.empty())
      channels = channelNames;
    for (auto *f : made) {
      candidate.emplace_back(f);
      candidate.back()->initialize(static_cast<float>(rate), maxFrames,
                                   channels);
    }
  }
  if (in.bad())
    throw std::runtime_error("error reading config: " +
                             normalizedPath.string());
}

void Engine::process(float *samples, unsigned frames) {
  if (frames > maxFrames)
    throw std::runtime_error("frame block exceeds configured maximum");
  for (unsigned c = 0; c < channelCount; ++c) {
    channelPtrs[c] = planar[c].data();
    scratchPtrs[c] = scratch[c].data();
  }
  for (unsigned c = 0; c < channelCount; ++c)
    for (unsigned f = 0; f < frames; ++f)
      planar[c][f] = samples[f * channelCount + c];
  for (auto &filter : filters) {
    float **output =
        filter->getInPlace() ? channelPtrs.data() : scratchPtrs.data();
    filter->process(output, channelPtrs.data(), frames);
    if (!filter->getInPlace())
      std::swap(channelPtrs, scratchPtrs);
  }
  for (unsigned c = 0; c < channelCount; ++c)
    for (unsigned f = 0; f < frames; ++f)
      samples[f * channelCount + c] = channelPtrs[c][f];
}
