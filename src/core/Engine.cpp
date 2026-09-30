#include "Engine.h"
#include "../plugin/IPluginFailureState.h"
#include "../plugin/IPluginBypassControl.h"
#include "../plugin/IPluginParameterControl.h"
#include "../plugin/IPluginIdentity.h"
#include "../plugin/IPluginLatencyState.h"
#include "../plugin/IPluginSourceContext.h"
#include "../plugin/IPluginStatePersistence.h"
#include "FilterConfiguration.h"
#include "FilterConfigurationContext.h"

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
#include <fstream>
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
               bool enablePluginFilters)
    : rate(sampleRate), channelCount(channels), maxFrameCount(maxFrames),
      allowPendingEndpointVolume(allowPendingEndpointVolume) {
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
        for (auto *filter : produced)
          candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                               source, line, directive, includeChain});
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
  try {
    newGraph = buildGraph(candidate);
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

  graph.swap(newGraph);
  configuration.swap(newConfiguration);
  descriptions.swap(newDescriptions);
  loadedConfigFiles.swap(configFiles);
  fixedBlock = newFixedBlock;
}

std::vector<Engine::FilterNode> Engine::buildGraph(FilterList &candidate) {
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
                             std::vector<std::filesystem::path> &configFiles,
                             std::vector<std::filesystem::path> &attemptedFiles,
                             std::vector<IncludeSite> &includeChain,
                             bool &stageActive,
                             mup::ParserX *expressionParser) {
  std::error_code ec;
  auto absolutePath = std::filesystem::absolute(configPath, ec);
  if (ec)
    throw std::runtime_error("cannot resolve config path '" +
                             configPath.string() + "': " + ec.message());
  auto normalizedPath = std::filesystem::weakly_canonical(absolutePath, ec);
  if (ec)
    normalizedPath = absolutePath.lexically_normal();
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

  std::ifstream in(normalizedPath);
  if (!in)
    throw std::runtime_error("cannot open config: " + normalizedPath.string());
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
  for (auto &factory : factories) {
    auto produced = factory->startOfFile(widePath);
    for (auto *filter : produced)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, 0, "file initialization",
                           includeChain});
  }
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
  while (std::getline(in, raw)) {
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
      const bool condition = parentActive && evaluateCondition(params, lineNo);
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
                             evaluateCondition(params, lineNo);
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
      std::filesystem::path included(StringHelper::toString(params, 65001));
      if (included.is_relative())
        included = normalizedPath.parent_path() / included;
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
    if (command.empty())
      continue;
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
    for (auto *filter : made)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, lineNo,
                           StringHelper::toString(originalCommand, 65001),
                           includeChain});
  }
  if (!conditions.empty())
    throw std::runtime_error(normalizedPath.string() + ":" +
                             std::to_string(conditions.back().openingLine) +
                             ": If was not closed by EndIf");
  if (in.bad())
    throw std::runtime_error("error reading config: " +
                             normalizedPath.string());
  for (auto &factory : factories) {
    auto produced = factory->endOfFile(widePath);
    for (auto *filter : produced)
      candidate.push_back({std::unique_ptr<IFilter, FilterDeleter>(filter),
                           normalizedPath, lineNo, "file finalization",
                           includeChain});
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

  // `FilterConfiguration::read` copies the complete input into its own
  // preallocated planes. Read both graphs before writing the mixed output so
  // an in-place interleaved buffer remains the same source for each graph.
  configuration->read(samples, frames);
  configuration->process(frames);
  next.configuration->read(samples, frames);
  next.configuration->process(frames);
  transitionCounter = configuration->doTransition(
      next.configuration.get(), frames, transitionCounter, transitionLength);
  configuration->write(samples, frames);
  return transitionCounter;
}

std::vector<std::string> Engine::failedPluginDescriptions() const {
  std::vector<std::string> failures;
  for (size_t i = 0; i < graph.size(); ++i) {
    const auto *state =
        dynamic_cast<const IPluginFailureState *>(graph[i].filter);
    if (state && state->processingFailed() && i < descriptions.size())
      failures.push_back(state->failureIdentifier() + " — " + descriptions[i]);
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
