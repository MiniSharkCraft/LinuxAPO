#include "Engine.h"
#include "DevicePatternMatcher.h"
#include "../plugin/IPluginFailureState.h"
#include "../plugin/IPluginBypassControl.h"
#include "../plugin/IPluginParameterControl.h"
#include "../plugin/IPluginIdentity.h"
#include "../plugin/IPluginLatencyState.h"
#include "../plugin/IPluginLatencyRefresh.h"
#include "../plugin/IPluginSourceContext.h"
#include "../plugin/IPluginStatePersistence.h"
#include "../plugin/PluginLatencyLimits.h"
#include "FilterConfiguration.h"
#include "FilterConfigurationContext.h"
#include "UpstreamFilterEngineProcess.h"
#include "ConfigSource.h"

#include "BiQuadFilterFactory.h"
#include "BiQuadFilter.h"
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
#include "PreampFilter.h"
#include "LoudnessCorrectionFilterFactory.h"
#include "LoudnessCorrectionFilter.h"
#include "LoudnessVolumeProvider.h"
#ifdef SKYAPO_HAVE_MUPARSERX
#include "UpstreamRegexFunctions.h"
#include "LogicalOperators.h"
#include "StringOperators.h"
#endif
#ifdef SKYAPO_HAVE_LV2
#include "LV2PluginHost.h"
#endif
#ifdef SKYAPO_HAVE_CLAP
#include "CLAPPluginHost.h"
#endif
#ifdef SKYAPO_HAVE_VST3
#include "VST3PluginHost.h"
#endif
#ifdef SKYAPO_HAVE_FST_VST2
#include "VST2PluginFilter.h"
#endif
#include "helpers/ChannelHelper.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifdef SKYAPO_HAVE_MUPARSERX
#include <mpPackageCommon.h>
#include <mpPackageNonCmplx.h>
#include <mpPackageStr.h>
#include <mpParser.h>
#elif defined(SKYAPO_HAVE_MUPARSER)
#include <muParser.h>
#endif

namespace {
// Bound aggregate PDC storage as well as each individual delay. A valid but
// adversarial Copy fan-in can otherwise create thousands of individually
// bounded rings and exhaust memory while loading or refreshing a graph.
constexpr uint64_t MaxPdcRingSamples = 4ULL * 1024ULL * 1024ULL;

void accountPdcRing(uint64_t &usedSamples, uint64_t ringSamples) {
  if (ringSamples > MaxPdcRingSamples - usedSamples)
    throw std::runtime_error(
        "PDC compensation exceeds SkyAPO's 16 MiB realtime ring-buffer "
        "budget");
  usedSamples += ringSamples;
}

class PdcAlignmentFilter final : public IFilter {
public:
  explicit PdcAlignmentFilter(std::vector<unsigned> channels)
      : channels_(std::move(channels)) {}

  bool getInPlace() override {
    return false;
  }
  std::vector<std::wstring>
  initialize(float, unsigned, std::vector<std::wstring> names) override {
    delays_.assign(names.size(), 0);
    rings_.resize(names.size());
    cursors_.assign(names.size(), 0);
    return names;
  }
  void configure(const std::vector<uint64_t> &arrival,
                 std::vector<uint64_t> &updated,
                 uint64_t &pdcRingSamples) {
    uint64_t latest = 0;
    for (const unsigned channel : channels_)
      if (channel < arrival.size())
        latest = std::max(latest, arrival[channel]);
    for (const unsigned channel : channels_) {
      if (channel >= delays_.size() || channel >= arrival.size())
        continue;
      const uint64_t difference = latest - arrival[channel];
      if (difference > skyapo::plugin::MaxRealtimeLatencySamples)
        throw std::runtime_error(
            "PDC delay exceeds SkyAPO's realtime compensation safety limit");
    }
    for (const unsigned channel : channels_) {
      if (channel >= delays_.size() || channel >= arrival.size())
        continue;
      const auto difference = static_cast<uint32_t>(latest - arrival[channel]);
      accountPdcRing(pdcRingSamples, difference);
      setDelay(channel, static_cast<uint32_t>(difference));
      updated[channel] = latest;
    }
  }
  void process(float **output, float **input, unsigned frames) override {
    for (unsigned channel = 0; channel < delays_.size(); ++channel) {
      const uint32_t delay = delays_[channel];
      if (!delay) {
        std::copy_n(input[channel], frames, output[channel]);
        continue;
      }
      auto &ring = rings_[channel];
      size_t &cursor = cursors_[channel];
      for (unsigned frame = 0; frame < frames; ++frame) {
        const float delayed = ring[cursor];
        ring[cursor] = input[channel][frame];
        output[channel][frame] = delayed;
        if (++cursor == ring.size())
          cursor = 0;
      }
    }
  }

private:
  void setDelay(unsigned channel, uint32_t delay) {
    if (delays_[channel] == delay)
      return;
    delays_[channel] = delay;
    rings_[channel].assign(delay, 0.0f);
    cursors_[channel] = 0;
  }

  std::vector<unsigned> channels_;
  std::vector<uint32_t> delays_;
  std::vector<std::vector<float>> rings_;
  std::vector<size_t> cursors_;
};

class PdcCopyFilter final : public CopyFilter {
public:
  explicit PdcCopyFilter(const std::vector<Assignment> &assignments)
      : CopyFilter(assignments) {}

  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrames,
             std::vector<std::wstring> names) override {
    auto outputs = CopyFilter::initialize(sampleRate, maxFrames, names);
    channelNames_ = std::move(names);
    targets_.clear();
    for (const auto &assignment : getAssignments()) {
      Target target;
      target.output = ChannelHelper::getChannelIndex(assignment.targetChannel,
                                                     outputs, true);
      if (target.output < 0)
        throw std::runtime_error("Copy target channel could not be resolved");
      for (const auto &summand : assignment.sourceSum) {
        Term term;
        term.channel = summand.channel.empty()
                           ? -1
                           : ChannelHelper::getChannelIndex(summand.channel,
                                                            channelNames_);
        if (!summand.channel.empty() && term.channel < 0)
          throw std::runtime_error("Copy source channel could not be resolved");
        term.constant = summand.channel.empty();
        term.factor = static_cast<float>(
            summand.isDecibel ? std::pow(10.0, summand.factor / 20.0)
                              : summand.factor);
        target.terms.push_back(std::move(term));
      }
      targets_.push_back(std::move(target));
    }
    return outputs;
  }

  void configure(const std::vector<uint64_t> &arrival,
                 std::vector<uint64_t> &updated,
                 uint64_t &pdcRingSamples) {
    // Validate all fan-in delay lengths before changing any ring in this
    // filter. A rejected graph/latency update must not leave half the fan-in
    // scheduled with its previous compensation.
    for (const auto &target : targets_) {
      uint64_t latest = 0;
      for (const auto &term : target.terms)
        if (!term.constant &&
            static_cast<size_t>(term.channel) < arrival.size())
          latest = std::max(latest, arrival[term.channel]);
      for (const auto &term : target.terms) {
        if (term.constant ||
            static_cast<size_t>(term.channel) >= arrival.size())
          continue;
        if (latest - arrival[term.channel] >
            skyapo::plugin::MaxRealtimeLatencySamples)
          throw std::runtime_error("PDC Copy delay exceeds SkyAPO's realtime "
                                   "compensation safety limit");
      }
    }
    for (auto &target : targets_) {
      uint64_t latest = 0;
      for (const auto &term : target.terms)
        if (!term.constant &&
            static_cast<size_t>(term.channel) < arrival.size())
          latest = std::max(latest, arrival[term.channel]);
      if (target.output < 0 ||
          static_cast<size_t>(target.output) >= updated.size())
        continue;
      for (auto &term : target.terms) {
        uint32_t delay = 0;
        if (!term.constant &&
            static_cast<size_t>(term.channel) < arrival.size()) {
          const uint64_t difference = latest - arrival[term.channel];
          if (difference > skyapo::plugin::MaxRealtimeLatencySamples)
            throw std::runtime_error("PDC Copy delay exceeds SkyAPO's realtime "
                                     "compensation safety limit");
          delay = static_cast<uint32_t>(difference);
        }
        accountPdcRing(pdcRingSamples, delay);
        if (term.delay != delay) {
          term.delay = delay;
          term.ring.assign(delay, 0.0f);
          term.cursor = 0;
        }
      }
      updated[target.output] = latest;
    }
  }

  void process(float **output, float **input, unsigned frames) override {
    for (auto &target : targets_) {
      if (target.terms.empty())
        continue;
      for (unsigned frame = 0; frame < frames; ++frame) {
        float sum = sample(target.terms.front(), input, frame);
        sum *= target.terms.front().factor;
        for (size_t index = 1; index < target.terms.size(); ++index) {
          auto &term = target.terms[index];
          sum += sample(term, input, frame) * term.factor;
        }
        output[target.output][frame] = sum;
      }
    }
  }

private:
  struct Term {
    int channel{-1};
    float factor{1.0f};
    bool constant{};
    uint32_t delay{};
    std::vector<float> ring;
    size_t cursor{};
  };
  struct Target {
    int output{-1};
    std::vector<Term> terms;
  };
  static float sample(Term &term, float **input, unsigned frame) {
    if (term.constant)
      return 1.0f;
    if (!term.delay)
      return input[term.channel][frame];
    const float value = term.ring[term.cursor];
    term.ring[term.cursor] = input[term.channel][frame];
    if (++term.cursor == term.ring.size())
      term.cursor = 0;
    return value;
  }

  std::vector<std::wstring> channelNames_;
  std::vector<Target> targets_;
};

template <typename T, typename... Args> T *allocateFilter(Args &&...args) {
  void *memory = MemoryHelper::alloc(sizeof(T));
  try {
    return new (memory) T(std::forward<Args>(args)...);
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

void stripInlineComment(std::string &line) {
  bool quoted = false;
  bool escaped = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '#' && !quoted) {
      line.resize(i);
      return;
    }
    if (ch == '"' && !escaped)
      quoted = !quoted;
    if (quoted && ch == '\\' && !escaped)
      escaped = true;
    else
      escaped = false;
  }
}

#ifdef SKYAPO_HAVE_MUPARSERX
std::wstring evaluateExpression(mup::ParserX &parser,
                                const std::wstring &expression,
                                const std::filesystem::path &path,
                                unsigned line, const char *context) {
  try {
    parser.SetExpr(expression);
    const mup::IValue &result = parser.Eval();
    if (result.GetType() == 's')
      return result.GetString();
    if (result.GetType() == 'b')
      return result.ToString();
    if (!std::isfinite(result.GetFloat()))
      throw std::runtime_error("expression result is not finite");
    return result.ToString();
  } catch (const mup::ParserError &e) {
    throw std::runtime_error(path.string() + ":" + std::to_string(line) +
                             ": invalid " + context + ": " +
                             StringHelper::toString(e.GetMsg(), 65001));
  } catch (const std::exception &e) {
    throw std::runtime_error(path.string() + ":" + std::to_string(line) +
                             ": invalid " + context + ": " + e.what());
  }
}

std::wstring expandInlineExpressions(mup::ParserX &parser,
                                     const std::wstring &input,
                                     const std::filesystem::path &path,
                                     unsigned line) {
  std::wstring output;
  std::wstring expression;
  bool inExpression = false;
  bool escaped = false;
  for (wchar_t character : input) {
    if (character == L'`') {
      if (escaped) {
        (inExpression ? expression : output) += character;
      } else if (inExpression) {
        output += evaluateExpression(parser, expression, path, line,
                                     "inline expression");
        expression.clear();
        inExpression = false;
      } else {
        inExpression = true;
      }
      escaped = false;
      continue;
    }
    if (character == L'\\') {
      if (escaped) {
        (inExpression ? expression : output) += character;
        escaped = false;
      } else {
        escaped = true;
      }
      continue;
    }
    if (escaped) {
      (inExpression ? expression : output) += L'\\';
      escaped = false;
    }
    (inExpression ? expression : output) += character;
  }
  if (escaped)
    (inExpression ? expression : output) += L'\\';
  if (inExpression)
    throw std::runtime_error(path.string() + ":" + std::to_string(line) +
                             ": unterminated inline expression (missing `)");
  return output;
}
#endif
} // namespace

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
               std::vector<std::wstring> names, bool allowPendingEndpointVolume,
               bool enablePluginFilters, std::wstring deviceMatchText)
    : rate(sampleRate), channelCount(channels), maxFrameCount(maxFrames),
      allowPendingEndpointVolume(allowPendingEndpointVolume),
      deviceMatchText(std::move(deviceMatchText)) {
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
  factories.push_back(std::make_unique<LoudnessCorrectionFilterFactory>());
#ifdef SKYAPO_HAVE_CLAP
  if (enablePluginFilters)
    factories.push_back(makeCLAPPluginFilterFactory());
#endif
#ifdef SKYAPO_HAVE_VST3
  if (enablePluginFilters)
    factories.push_back(makeVST3PluginFilterFactory());
#endif
#ifdef SKYAPO_HAVE_FST_VST2
  if (enablePluginFilters)
    factories.push_back(makeVST2PluginFilterFactory());
#endif
#ifdef SKYAPO_HAVE_LV2
  if (enablePluginFilters)
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
  std::vector<std::filesystem::path> configFiles;
  std::vector<std::filesystem::path> attemptedFiles;
  std::vector<IncludeSite> includeChain;
  bool stageActive = true;
#ifdef SKYAPO_HAVE_MUPARSERX
  mup::ParserX expressionParser(mup::pckALL_NON_COMPLEX);
  expressionParser.EnableAutoCreateVar(true);
  expressionParser.DefineConst(L"sampleRate",
                               static_cast<mup::float_type>(rate));
  expressionParser.DefineConst(L"inputChannelCount",
                               static_cast<mup::float_type>(channelCount));
  expressionParser.DefineConst(L"outputChannelCount",
                               static_cast<mup::float_type>(channelCount));
  expressionParser.DefineFun(new skyapo::config::RegexSearchFunction());
  expressionParser.DefineFun(new skyapo::config::RegexReplaceFunction());
  // These are the actual Equalizer APO operators used by its
  // ExpressionFilterFactory: '+' concatenates when either side is a string,
  // and 'not' supplies the EAPO boolean prefix operator.
  expressionParser.RemoveOprt(L"+");
  expressionParser.DefineOprt(new AddOperator());
  expressionParser.DefineInfixOprt(new NotOperator());
  auto *expressionParserPtr = &expressionParser;
#else
  mup::ParserX *expressionParserPtr = nullptr;
#endif
  FilterConfigurationContext context(channelCount, channelCount, maxFrameCount);
  const auto addReturnedFilters =
      [&](std::vector<IFilter *> produced, const std::filesystem::path &source,
          unsigned line, const std::string &directive) {
        struct Guard {
          std::vector<IFilter *> &filters;
          FilterDeleter deleter;
          ~Guard() {
            for (auto *filter : filters)
              deleter(filter);
          }
        } guard{produced, {}};
        for (auto &filter : produced) {
          std::unique_ptr<IFilter, FilterDeleter> owned(filter);
          filter = nullptr;
          candidate.push_back(
              {std::move(owned), source, line, directive, includeChain});
        }
      };
  for (auto &factory : factories) {
    factory->initialize(&context);
    addReturnedFilters(factory->startOfConfiguration(), path, 0,
                       "configuration initialization");
  }
  try {
    parseConfigFile(std::filesystem::path(path), candidate, includeStack,
                    configFiles, attemptedFiles, includeChain, stageActive,
                    expressionParserPtr);
  } catch (const ConfigError &) {
    throw;
  } catch (const std::exception &error) {
    if (includeChain.empty())
      throw;
    throw ConfigError(error.what(), includeChain, attemptedFiles);
  }
  for (auto &factory : factories)
    addReturnedFilters(factory->endOfConfiguration(), path, 0,
                       "configuration finalization");

  std::vector<FilterNode> newGraph;
  std::vector<std::unique_ptr<IFilter, FilterDeleter>> generatedFilters;
  try {
    newGraph = buildGraph(candidate, generatedFilters);
  } catch (const std::exception &error) {
    const std::string message = error.what();
    for (const auto &parsed : candidate) {
      const std::string prefix =
          parsed.source.string() + ":" + std::to_string(parsed.line) + ":";
      if (message.rfind(prefix, 0) == 0 && !parsed.includeChain.empty())
        throw ConfigError(message, parsed.includeChain);
    }
    throw;
  }
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
  unsigned allChannelCount = static_cast<unsigned>(channelNames.size());
  for (const auto &node : newGraph) {
    for (const unsigned channel : node.inputs)
      allChannelCount = std::max(allChannelCount, channel + 1);
    for (const unsigned channel : node.outputs)
      allChannelCount = std::max(allChannelCount, channel + 1);
  }
  rebuildPdcPlan(newGraph, allChannelCount);
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

  void *memory = MemoryHelper::alloc(sizeof(FilterConfiguration));
  FilterConfiguration *built = nullptr;
  try {
    built = new (memory) FilterConfiguration(&context, infos, allChannelCount);
  } catch (...) {
    MemoryHelper::free(memory);
    freeInfos();
    throw;
  }
  std::unique_ptr<FilterConfiguration, ConfigurationDeleter> newConfiguration(
      built);
  for (auto &parsed : candidate)
    parsed.filter.release();
  for (auto &filter : generatedFilters)
    filter.release();

  graph.swap(newGraph);
  this->allChannelCount = allChannelCount;
  configuration.swap(newConfiguration);
  descriptions.swap(newDescriptions);
  loadedConfigFiles.swap(configFiles);
  fixedBlock = newFixedBlock;
}

std::vector<Engine::FilterNode> Engine::buildGraph(
    FilterList &candidate,
    std::vector<std::unique_ptr<IFilter, FilterDeleter>> &generated) {
  std::vector<std::wstring> allNames = channelNames;
  std::vector<std::wstring> selectedNames = allNames;
  std::vector<FilterNode> result;
  result.reserve(candidate.size());

  for (auto &parsed : candidate) {
    IFilter *filter = parsed.filter.get();
    const auto location =
        parsed.source.string() + ":" + std::to_string(parsed.line) + ": ";
    if (dynamic_cast<LoudnessCorrectionFilter *>(filter) &&
        !allowPendingEndpointVolume &&
        !skyapo::platform::LoudnessVolumeProvider::available())
      throw std::runtime_error(
          location + "LoudnessCorrection needs a live PipeWire endpoint-volume "
                     "snapshot; the offline renderer/config checker has none");
    if (auto *preamp = dynamic_cast<PreampFilter *>(filter)) {
      const double db = preamp->getDbGain();
      const double linear = std::pow(10.0, db / 20.0);
      if (!std::isfinite(db) || !std::isfinite(linear) ||
          linear > std::numeric_limits<float>::max())
        throw std::runtime_error(location +
                                 "Preamp gain is outside the finite float "
                                 "audio range");
    }
    if (auto *biquad = dynamic_cast<BiQuadFilter *>(filter)) {
      const double frequency = biquad->getFreq();
      const double shape = biquad->getBandwidthOrQOrS();
      const double gain = biquad->getDbGain();
      const bool isShelf = biquad->getType() == BiQuad::LOW_SHELF ||
                           biquad->getType() == BiQuad::HIGH_SHELF;
      const bool gainIsUsed = biquad->getType() == BiQuad::PEAKING || isShelf;
      const double linearGain = std::pow(10.0, gain / 40.0);
      bool shapeValid = std::isfinite(shape) && shape > 0.0;
      if (isShelf && biquad->getIsBandwidthOrS())
        shapeValid = shapeValid && shape <= 12.0;
      if (!std::isfinite(frequency) || frequency <= 0.0 ||
          frequency >= static_cast<double>(rate) / 2.0)
        throw std::runtime_error(location +
                                 "Filter frequency must be above 0 Hz and "
                                 "below the Nyquist frequency (" +
                                 std::to_string(rate / 2.0) + " Hz)");
      if (!std::isfinite(gain) ||
          (gainIsUsed && (!std::isfinite(linearGain) ||
                          linearGain > std::numeric_limits<float>::max())) ||
          !shapeValid)
        throw std::runtime_error(
            location +
            "Filter gain and Q/bandwidth/slope must be finite and in range");
    }
    const auto savedSelection = selectedNames;
    if (filter->getAllChannels())
      selectedNames = allNames;
    const auto inputNames = selectedNames;
    if (auto *copy = dynamic_cast<CopyFilter *>(filter)) {
      auto *adapted = allocateFilter<PdcCopyFilter>(copy->getAssignments());
      parsed.filter.reset(adapted);
      filter = adapted;
    }
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
    const bool pluginNode =
        dynamic_cast<IPluginLatencyState *>(filter) != nullptr;
    if (pluginNode) {
      std::vector<unsigned> alignChannels;
      alignChannels.reserve(inputNames.size());
      for (const auto &name : inputNames) {
        const auto it = std::find(allNames.begin(), allNames.end(), name);
        alignChannels.push_back(static_cast<unsigned>(it - allNames.begin()));
      }
      auto *align = allocateFilter<PdcAlignmentFilter>(alignChannels);
      std::vector<std::wstring> alignNames(allNames.begin(), allNames.end());
      const auto alignOutputs = align->initialize(static_cast<float>(rate),
                                                  maxFrameCount, alignNames);
      std::unique_ptr<IFilter, FilterDeleter> alignOwner(align);
      generated.push_back(std::move(alignOwner));
      FilterNode alignNode;
      alignNode.filter = align;
      alignNode.pdcAlignment = true;
      alignNode.alignmentChannels = std::move(alignChannels);
      alignNode.inPlace = false;
      alignNode.fixedBlock = false;
      for (const auto &name : alignOutputs) {
        const auto it = std::find(allNames.begin(), allNames.end(), name);
        const unsigned channel = static_cast<unsigned>(it - allNames.begin());
        alignNode.inputs.push_back(channel);
        alignNode.outputs.push_back(channel);
      }
      result.push_back(std::move(alignNode));
    }
    for (const auto &name : outputNames) {
      auto it = std::find(allNames.begin(), allNames.end(), name);
      if (it == allNames.end()) {
        node.outputs.push_back(static_cast<unsigned>(allNames.size()));
        allNames.push_back(name);
      } else {
        node.outputs.push_back(static_cast<unsigned>(it - allNames.begin()));
      }
    }
    node.filter = filter;
    node.description = parsed.directive + " — " + parsed.source.string() + ":" +
                       std::to_string(parsed.line);
    result.push_back(std::move(node));
    selectedNames =
        filter->getSelectChannels() ? std::move(outputNames) : savedSelection;
  }

  const bool hasLatencyAwarePlugin =
      std::any_of(result.begin(), result.end(), [](const FilterNode &node) {
        return dynamic_cast<IPluginLatencyState *>(node.filter) != nullptr;
      });
  if (hasLatencyAwarePlugin) {
    std::vector<unsigned> physicalChannels(channelCount);
    for (unsigned channel = 0; channel < channelCount; ++channel)
      physicalChannels[channel] = channel;
    auto *alignment = allocateFilter<PdcAlignmentFilter>(physicalChannels);
    alignment->initialize(static_cast<float>(rate), maxFrameCount, allNames);
    generated.emplace_back(alignment);
    FilterNode outputAlignment;
    outputAlignment.filter = alignment;
    outputAlignment.pdcAlignment = true;
    outputAlignment.alignmentChannels = std::move(physicalChannels);
    outputAlignment.inPlace = false;
    outputAlignment.fixedBlock = false;
    for (size_t channel = 0; channel < allNames.size(); ++channel) {
      outputAlignment.inputs.push_back(static_cast<unsigned>(channel));
      outputAlignment.outputs.push_back(static_cast<unsigned>(channel));
    }
    result.push_back(std::move(outputAlignment));
  }

  return result;
}

void Engine::rebuildPdcPlan(std::vector<FilterNode> &nodes,
                            unsigned laneCount) const {
  std::vector<uint64_t> arrival(laneCount, 0);
  uint64_t pdcRingSamples = 0;
  for (auto &node : nodes) {
    if (node.pdcAlignment) {
      auto *alignment = dynamic_cast<PdcAlignmentFilter *>(node.filter);
      if (!alignment)
        throw std::runtime_error("invalid internal PDC alignment node");
      const auto inputArrival = arrival;
      alignment->configure(inputArrival, arrival, pdcRingSamples);
      continue;
    }

    if (auto *copy = dynamic_cast<PdcCopyFilter *>(node.filter)) {
      const auto inputArrival = arrival;
      copy->configure(inputArrival, arrival, pdcRingSamples);
      continue;
    }

    uint64_t latestInput = 0;
    for (const unsigned input : node.inputs)
      if (input < arrival.size())
        latestInput = std::max(latestInput, arrival[input]);
    const auto *plugin = dynamic_cast<const IPluginLatencyState *>(node.filter);
    const uint64_t pluginLatency = plugin ? plugin->latencySamples() : 0;
    if (pluginLatency > skyapo::plugin::MaxRealtimeLatencySamples)
      throw std::runtime_error("PDC plugin latency exceeds SkyAPO's realtime "
                               "compensation safety limit: " +
                               node.description);
    for (size_t output = 0; output < node.outputs.size(); ++output) {
      const unsigned destination = node.outputs[output];
      if (destination >= arrival.size())
        continue;
      uint64_t sourceLatency = latestInput;
      const auto matchingInput =
          std::find(node.inputs.begin(), node.inputs.end(), destination);
      if (matchingInput != node.inputs.end())
        sourceLatency = arrival[destination];
      if (plugin && output < node.inputs.size() &&
          output < node.outputs.size() && node.inputs[output] < arrival.size())
        sourceLatency = arrival[node.inputs[output]];
      if (pluginLatency > std::numeric_limits<uint64_t>::max() - sourceLatency)
        throw std::runtime_error("PDC plugin latency overflow");
      arrival[destination] = sourceLatency + pluginLatency;
    }
  }
}

void Engine::parseConfigFile(const std::filesystem::path &configPath,
                             FilterList &candidate,
                             std::vector<std::filesystem::path> &includeStack,
                             std::vector<std::filesystem::path> &configFiles,
                             std::vector<std::filesystem::path> &attemptedFiles,
                             std::vector<IncludeSite> &includeChain,
                             bool &stageActive,
                             mup::ParserX *expressionParser) {
  const auto normalizedPath =
      skyapo::platform::ConfigSource::canonicalize(configPath);
  if (std::find(attemptedFiles.begin(), attemptedFiles.end(), normalizedPath) ==
      attemptedFiles.end())
    attemptedFiles.push_back(normalizedPath);
  if (includeStack.size() >= 100)
    throw std::runtime_error("include nesting exceeds 100 files at " +
                             normalizedPath.string());
  if (std::find(includeStack.begin(), includeStack.end(), normalizedPath) !=
      includeStack.end())
    throw std::runtime_error("include cycle detected at " +
                             normalizedPath.string());

  auto in = skyapo::platform::ConfigSource::open(normalizedPath);
  if (std::find(configFiles.begin(), configFiles.end(), normalizedPath) ==
      configFiles.end())
    configFiles.push_back(normalizedPath);
  includeStack.push_back(normalizedPath);
  struct PopPath {
    std::vector<std::filesystem::path> &stack;
    ~PopPath() {
      stack.pop_back();
    }
  } popPath{includeStack};

  const auto widePath = StringHelper::toWString(normalizedPath.string(), 65001);
  const auto addFileFilters = [&](std::vector<IFilter *> produced,
                                  unsigned line, const std::string &directive) {
    struct Guard {
      std::vector<IFilter *> &filters;
      FilterDeleter deleter;
      ~Guard() {
        for (auto *filter : filters)
          deleter(filter);
      }
    } guard{produced, {}};
    for (auto &filter : produced) {
      std::unique_ptr<IFilter, FilterDeleter> owned(filter);
      filter = nullptr;
      candidate.push_back(
          {std::move(owned), normalizedPath, line, directive, includeChain});
    }
  };
  for (auto &factory : factories)
    addFileFilters(factory->startOfFile(widePath), 0, "file initialization");
  std::string raw;
  unsigned lineNo = 0;
  struct ConditionalFrame {
    bool parentActive;
    bool branchTaken;
    bool active;
    bool sawElse;
    unsigned openingLine;
  };
  std::vector<ConditionalFrame> conditions;
  bool deviceMatches = true;
  const auto evaluateCondition = [&](const std::wstring &expression,
                                     unsigned conditionLine) -> bool {
#ifdef SKYAPO_HAVE_MUPARSERX
    try {
      expressionParser->SetExpr(expression);
      const mup::IValue &value = expressionParser->Eval();
      if (value.GetType() == 'b')
        return value.GetBool();
      const double numeric = value.GetFloat();
      if (!std::isfinite(numeric))
        throw std::runtime_error("expression result is not finite");
      return numeric != 0.0;
    } catch (const mup::ParserError &e) {
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(conditionLine) +
                               ": invalid If expression: " +
                               StringHelper::toString(e.GetMsg(), 65001));
    } catch (const std::exception &e) {
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(conditionLine) +
                               ": invalid If expression: " + e.what());
    }
#elif defined(SKYAPO_HAVE_MUPARSER)
    try {
      mu::Parser parser;
      parser.DefineConst("sampleRate", static_cast<double>(rate));
      parser.DefineConst("inputChannelCount",
                         static_cast<double>(channelCount));
      parser.DefineConst("outputChannelCount",
                         static_cast<double>(channelCount));
      parser.SetExpr(StringHelper::toString(expression, 65001));
      const double value = parser.Eval();
      if (!std::isfinite(value))
        throw std::runtime_error("expression result is not finite");
      return value != 0.0;
    } catch (const mu::Parser::exception_type &e) {
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(conditionLine) +
                               ": invalid If expression: " + e.GetMsg());
    } catch (const std::exception &e) {
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(conditionLine) +
                               ": invalid If expression: " + e.what());
    }
#else
    (void)expression;
    throw std::runtime_error(normalizedPath.string() + ":" +
                             std::to_string(conditionLine) +
                             ": If/ElseIf requires MuParserX or muParser "
                             "expression support; enable a parser backend and "
                             "rebuild SkyAPO");
#endif
  };
  while (in.readLine(raw)) {
    ++lineNo;
    stripInlineComment(raw);
    auto line = StringHelper::trim(StringHelper::toWString(raw, 65001));
    if (line.empty())
      continue;
    const auto colon = line.find(L':');
    if (colon == line.npos)
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": expected command:");
    std::wstring command = StringHelper::trim(line.substr(0, colon));
    std::wstring params = StringHelper::trim(line.substr(colon + 1));

    const bool parentActive = conditions.empty() || conditions.back().active;
    if (command == L"If") {
      const bool condition =
          parentActive && deviceMatches && evaluateCondition(params, lineNo);
      conditions.push_back(
          {parentActive, condition, parentActive && condition, false, lineNo});
      continue;
    }
    if (command == L"ElseIf") {
      if (conditions.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": ElseIf without matching If");
      auto &frame = conditions.back();
      if (frame.sawElse)
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": ElseIf cannot follow Else");
      const bool condition = frame.parentActive && !frame.branchTaken &&
                             deviceMatches && evaluateCondition(params, lineNo);
      frame.active = condition;
      frame.branchTaken = frame.branchTaken || condition;
      continue;
    }
    if (command == L"Else") {
      if (!params.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Else does not accept parameters");
      if (conditions.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Else without matching If");
      auto &frame = conditions.back();
      if (frame.sawElse)
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": duplicate Else in If block");
      frame.sawElse = true;
      frame.active = frame.parentActive && !frame.branchTaken;
      frame.branchTaken = true;
      continue;
    }
    if (command == L"EndIf") {
      if (!params.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": EndIf does not accept parameters");
      if (conditions.empty())
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": EndIf without matching If");
      conditions.pop_back();
      continue;
    }
    const bool conditionActive = conditions.empty() || conditions.back().active;
    if (!conditionActive)
      continue;

    if (command == L"Device") {
      deviceMatches =
          skyapo::core::devicePatternMatches(deviceMatchText, params);
      continue;
    }
    if (!deviceMatches)
      continue;

#ifdef SKYAPO_HAVE_MUPARSERX
    command = expandInlineExpressions(*expressionParser, command,
                                      normalizedPath, lineNo);
    params = expandInlineExpressions(*expressionParser, params, normalizedPath,
                                     lineNo);
    if (command == L"Eval") {
      (void)evaluateExpression(*expressionParser, params, normalizedPath,
                               lineNo, "Eval expression");
      continue;
    }
#else
    if (command == L"Eval" || command.find(L'`') != std::wstring::npos ||
        params.find(L'`') != std::wstring::npos)
      throw std::runtime_error(
          normalizedPath.string() + ":" + std::to_string(lineNo) +
          ": Eval/inline expressions require MuParserX support");
#endif

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
      const auto included = skyapo::platform::ConfigSource::resolveInclude(
          normalizedPath, StringHelper::toString(params, 65001));
      bool includedStage = stageActive;
      includeChain.push_back({normalizedPath, lineNo});
      // Keep the chain intact when recursion fails; loadConfig reports it as
      // parser-owned source ancestry. Successful recursion unwinds this site.
      parseConfigFile(included, candidate, includeStack, configFiles,
                      attemptedFiles, includeChain, includedStage,
                      expressionParser);
      includeChain.pop_back();
      continue;
    }

    const auto originalCommand = command;
    std::vector<IFilter *> made;
    for (auto &factory : factories) {
      if (auto *sourceContext =
              dynamic_cast<IPluginSourceContext *>(factory.get()))
        sourceContext->setPluginSourceLocation(normalizedPath, lineNo);
      made = factory->createFilter(widePath, command, params);
      if (!made.empty() || command.empty())
        break;
    }
    if (command.empty()) {
      for (auto *filter : made)
        FilterDeleter{}(filter);
      continue;
    }
    if (made.empty()) {
      const auto name = StringHelper::toString(originalCommand, 65001);
      const bool known =
          originalCommand == L"Preamp" || originalCommand == L"Delay" ||
          originalCommand == L"Channel" || originalCommand == L"Copy" ||
          originalCommand.rfind(L"Filter", 0) == 0 ||
          originalCommand == L"GraphicEQ" ||
          originalCommand == L"Convolution" || originalCommand == L"Plugin" ||
          originalCommand == L"LoudnessCorrection";
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
#if !defined(SKYAPO_HAVE_LV2) && !defined(SKYAPO_HAVE_CLAP)
      if (originalCommand == L"Plugin")
        throw std::runtime_error(normalizedPath.string() + ":" +
                                 std::to_string(lineNo) +
                                 ": Plugin requires "
                                 "native plugin host support");
#endif
      throw std::runtime_error(normalizedPath.string() + ":" +
                               std::to_string(lineNo) + ": invalid " + name +
                               " parameters");
    }
    addFileFilters(std::move(made), lineNo,
                   StringHelper::toString(originalCommand, 65001));
  }
  if (!conditions.empty())
    throw std::runtime_error(normalizedPath.string() + ":" +
                             std::to_string(conditions.back().openingLine) +
                             ": If was not closed by EndIf");
  if (in.failed())
    throw std::runtime_error("error reading config: " +
                             normalizedPath.string());
  for (auto &factory : factories)
    addFileFilters(factory->endOfFile(widePath), lineNo, "file finalization");
}

void Engine::process(float *samples, unsigned frames) {
  if (frames > maxFrameCount)
    throw std::runtime_error("frame block exceeds configured maximum");
  if (fixedBlock && frames != maxFrameCount)
    throw std::runtime_error(
        "convolution filters require the negotiated fixed audio block size");
  UpstreamFilterEngineProcess process(channelCount, channelCount,
                                      maxFrameCount);
  process.setConfigurations(configuration.get(), nullptr);
  process.process(samples, samples, frames);
}

unsigned Engine::processTransitionTo(Engine &next, float *samples,
                                     unsigned frames,
                                     unsigned transitionCounter,
                                     unsigned transitionLength) {
  if (this == &next)
    throw std::invalid_argument("cannot transition an Engine to itself");
  if (rate != next.rate || channelCount != next.channelCount ||
      maxFrameCount != next.maxFrameCount || channelNames != next.channelNames)
    throw std::invalid_argument("Engine transition formats do not match");
  if (!transitionLength || transitionCounter > transitionLength)
    throw std::invalid_argument("invalid Engine transition position");
  if (frames > maxFrameCount || frames > next.maxFrameCount)
    throw std::runtime_error("frame block exceeds transition capacity");
  if ((fixedBlock && frames != maxFrameCount) ||
      (next.fixedBlock && frames != next.maxFrameCount))
    throw std::runtime_error(
        "convolution filters require the negotiated fixed audio block size");

  UpstreamFilterEngineProcess process(channelCount, channelCount,
                                      transitionLength);
  process.setConfigurations(configuration.get(), next.configuration.get());
  process.setTransitionCounter(transitionCounter);
  process.process(samples, samples, frames);
  return process.takeTransitionComplete()
             ? process.getCompletedTransitionCounter()
             : process.getTransitionCounter();
}

std::vector<std::string> Engine::failedPluginDescriptions() const {
  std::vector<std::string> failures;
  for (size_t i = 0; i < graph.size(); ++i) {
    const auto *state =
        dynamic_cast<const IPluginFailureState *>(graph[i].filter);
    if (state && state->processingFailed())
      failures.push_back(state->failureIdentifier() + " — " +
                         graph[i].description);
  }
  return failures;
}

std::optional<uint64_t> Engine::pluginLatencySamples() const noexcept {
  uint64_t total = 0;
  for (const auto &node : graph) {
    if (!dynamic_cast<const IPluginFailureState *>(node.filter))
      continue;
    const auto *latency =
        dynamic_cast<const IPluginLatencyState *>(node.filter);
    if (!latency)
      return std::nullopt;
    const uint64_t samples = latency->latencySamples();
    if (samples > std::numeric_limits<uint64_t>::max() - total)
      return std::nullopt;
    total += samples;
  }
  return total;
}

bool Engine::pluginLatencyCompensationActive() const noexcept {
  return std::any_of(graph.begin(), graph.end(), [](const FilterNode &node) {
    return dynamic_cast<const IPluginLatencyState *>(node.filter) != nullptr;
  });
}

bool Engine::pluginLatencyRefreshPending() const noexcept {
  for (const auto &node : graph) {
    const auto *refresh =
        dynamic_cast<const IPluginLatencyRefresh *>(node.filter);
    if (refresh && refresh->latencyRefreshPending())
      return true;
  }
  return false;
}

unsigned Engine::refreshPluginLatencies() {
  unsigned refreshed = 0;
  for (const auto &node : graph) {
    auto *refresh = dynamic_cast<IPluginLatencyRefresh *>(node.filter);
    if (refresh && refresh->refreshPluginLatency())
      ++refreshed;
  }
  if (refreshed)
    rebuildPdcPlan(graph, allChannelCount);
  return refreshed;
}

unsigned Engine::savePersistentPluginStates() {
  unsigned saved = 0;
  for (const auto &node : graph) {
    auto *state = dynamic_cast<IPluginStatePersistence *>(node.filter);
    if (state && state->savePersistentPluginState())
      ++saved;
  }
  return saved;
}

void Engine::setPluginParameter(const std::string &pluginId,
                                const std::string &parameter, float value) {
  IPluginParameterControl *match = nullptr;
  for (const auto &node : graph) {
    auto *control = dynamic_cast<IPluginParameterControl *>(node.filter);
    if (!control || control->pluginIdentifier() != pluginId)
      continue;
    if (match)
      throw std::runtime_error("plugin ID is ambiguous in the active chain: " +
                               pluginId);
    match = control;
  }
  if (!match)
    throw std::runtime_error("no active plugin with live parameter control: " +
                             pluginId);
  match->setParameterValue(parameter, value);
}

void Engine::setPluginBypass(const std::string &pluginId, bool bypassed) {
  IPluginIdentity *identity = nullptr;
  IPluginBypassControl *bypassControl = nullptr;
  for (const auto &node : graph) {
    auto *candidate = dynamic_cast<IPluginIdentity *>(node.filter);
    if (!candidate || candidate->pluginIdentifier() != pluginId)
      continue;
    if (identity)
      throw std::runtime_error("plugin ID is ambiguous in the active chain: " +
                               pluginId);
    identity = candidate;
    bypassControl = dynamic_cast<IPluginBypassControl *>(node.filter);
  }
  if (!identity)
    throw std::runtime_error("no active plugin with bypass control: " +
                             pluginId);
  if (!bypassControl)
    throw std::runtime_error("plugin host does not support bypass: " +
                             pluginId);
  bypassControl->setPluginBypassed(bypassed);
}
