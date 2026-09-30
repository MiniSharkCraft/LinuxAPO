#include "VST3PluginHost.h"

#include "IFilter.h"
#include "IFilterFactory.h"
#include "IPluginFailureState.h"
#include "IPluginLatencyState.h"
#include "IPluginParameterControl.h"
#include "IPluginBypassControl.h"
#include "IPluginSourceContext.h"
#include "IPluginStatePersistence.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using VST3::Hosting::Module;
namespace fs = std::filesystem;
constexpr size_t MaxVST3StateBytes = 16 * 1024 * 1024;
constexpr std::array<uint8_t, 8> VST3StateMagic{'S', 'K', 'Y', 'V',
                                                'S', 'T', '3', '1'};

struct VST3StateData {
  std::string component;
  std::string controller;
};

uint64_t stateHash(std::string_view text) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t stateChecksum(std::string_view identity, std::string_view component,
                       std::string_view controller) {
  uint64_t hash = stateHash(identity);
  for (const auto segment : {component, controller}) {
    hash ^= 0xff;
    hash *= 1099511628211ull;
    for (const unsigned char byte : segment) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

void appendU32(std::vector<uint8_t> &bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void appendU64(std::vector<uint8_t> &bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
bool takeU32(const std::vector<uint8_t> &bytes, size_t &offset,
             uint32_t &value) {
  if (offset > bytes.size() || bytes.size() - offset < 4)
    return false;
  value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= uint32_t(bytes[offset++]) << (i * 8);
  return true;
}
bool takeU64(const std::vector<uint8_t> &bytes, size_t &offset,
             uint64_t &value) {
  if (offset > bytes.size() || bytes.size() - offset < 8)
    return false;
  value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= uint64_t(bytes[offset++]) << (i * 8);
  return true;
}

fs::path statePathFor(std::string_view identity) {
  if (identity.size() > 4096)
    throw std::runtime_error("VST3 state identity exceeds the 4096-byte limit");
  fs::path base;
  if (const char *xdg = std::getenv("XDG_STATE_HOME");
      xdg && *xdg && fs::path(xdg).is_absolute())
    base = xdg;
  else if (const char *home = std::getenv("HOME"); home && *home)
    base = fs::path(home) / ".local" / "state";
  else
    throw std::runtime_error(
        "VST3 state persistence needs XDG_STATE_HOME or HOME");
  std::ostringstream name;
  name << std::hex << std::setw(16) << std::setfill('0') << stateHash(identity)
       << ".vst3state";
  return base / "skyapo" / "vst3-state" / name.str();
}

std::vector<uint8_t> encodeState(std::string_view identity,
                                 const VST3StateData &state) {
  if (identity.size() > 4096 || state.component.size() > MaxVST3StateBytes ||
      state.controller.size() > MaxVST3StateBytes ||
      state.component.size() + state.controller.size() > MaxVST3StateBytes)
    throw std::runtime_error("VST3 plugin state exceeds the 16 MiB limit");
  std::vector<uint8_t> bytes;
  bytes.reserve(VST3StateMagic.size() + 4 + 8 * 4 + identity.size() +
                state.component.size() + state.controller.size());
  bytes.insert(bytes.end(), VST3StateMagic.begin(), VST3StateMagic.end());
  appendU32(bytes, 1);
  appendU64(bytes, identity.size());
  appendU64(bytes, state.component.size());
  appendU64(bytes, state.controller.size());
  appendU64(bytes, stateChecksum(identity, state.component, state.controller));
  bytes.insert(bytes.end(), identity.begin(), identity.end());
  bytes.insert(bytes.end(), state.component.begin(), state.component.end());
  bytes.insert(bytes.end(), state.controller.begin(), state.controller.end());
  return bytes;
}

VST3StateData decodeState(const std::vector<uint8_t> &bytes,
                          std::string_view expectedIdentity) {
  size_t offset = VST3StateMagic.size();
  uint32_t version{};
  uint64_t identitySize{}, componentSize{}, controllerSize{}, checksum{};
  if (bytes.size() < VST3StateMagic.size() ||
      !std::equal(VST3StateMagic.begin(), VST3StateMagic.end(),
                  bytes.begin()) ||
      !takeU32(bytes, offset, version) || version != 1 ||
      !takeU64(bytes, offset, identitySize) || identitySize > 4096 ||
      !takeU64(bytes, offset, componentSize) ||
      componentSize > MaxVST3StateBytes ||
      !takeU64(bytes, offset, controllerSize) ||
      controllerSize > MaxVST3StateBytes || !takeU64(bytes, offset, checksum) ||
      identitySize > bytes.size() - offset ||
      componentSize > bytes.size() - offset - identitySize ||
      controllerSize != bytes.size() - offset - identitySize - componentSize)
    throw std::runtime_error("corrupt VST3 state sidecar");
  const std::string identity(
      reinterpret_cast<const char *>(bytes.data() + offset),
      static_cast<size_t>(identitySize));
  offset += static_cast<size_t>(identitySize);
  if (identity != expectedIdentity)
    throw std::runtime_error("VST3 state sidecar identity mismatch");
  VST3StateData state;
  state.component.assign(reinterpret_cast<const char *>(bytes.data() + offset),
                         static_cast<size_t>(componentSize));
  offset += static_cast<size_t>(componentSize);
  state.controller.assign(reinterpret_cast<const char *>(bytes.data() + offset),
                          static_cast<size_t>(controllerSize));
  if (checksum != stateChecksum(identity, state.component, state.controller))
    throw std::runtime_error("VST3 state sidecar checksum mismatch");
  if (state.component.empty() && state.controller.empty())
    throw std::runtime_error("VST3 state sidecar has no plugin data");
  return state;
}

std::optional<VST3StateData> readStateFile(const fs::path &path,
                                           std::string_view identity) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT)
      return std::nullopt;
    throw std::runtime_error("cannot open VST3 state sidecar: " +
                             path.string() + ": " + std::strerror(errno));
  }
  struct FdOwner {
    int fd;
    ~FdOwner() {
      if (fd >= 0)
        close(fd);
    }
  } owner{fd};
  struct stat status{};
  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0 ||
      status.st_size <= 0 ||
      static_cast<uint64_t>(status.st_size) > MaxVST3StateBytes + 8192)
    throw std::runtime_error("invalid or oversized VST3 state sidecar");
  std::vector<uint8_t> bytes(static_cast<size_t>(status.st_size));
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("short read from VST3 state sidecar");
    offset += static_cast<size_t>(count);
  }
  uint8_t extra{};
  ssize_t count;
  do {
    count = read(fd, &extra, 1);
  } while (count < 0 && errno == EINTR);
  if (count != 0)
    throw std::runtime_error("VST3 state sidecar changed while being read");
  return decodeState(bytes, identity);
}

void writeAll(int fd, const uint8_t *data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const ssize_t count = write(fd, data + offset, size - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot write VST3 state sidecar");
    offset += static_cast<size_t>(count);
  }
}

void atomicWriteState(const fs::path &path, std::string_view identity,
                      const VST3StateData &state) {
  const auto bytes = encodeState(identity, state);
  const auto directory = path.parent_path();
  std::error_code error;
  fs::create_directories(directory, error);
  if (error || fs::is_symlink(fs::symlink_status(directory, error)) || error)
    throw std::runtime_error("cannot safely create VST3 state directory");
  if (chmod(directory.c_str(), 0700) < 0)
    throw std::runtime_error("cannot secure VST3 state directory");
  std::string pattern = path.string() + ".tmp-XXXXXX";
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  const int fd = mkstemp(temp.data());
  if (fd < 0)
    throw std::runtime_error("cannot create VST3 state temporary file");
  bool fdOpen = true;
  bool renamed = false;
  try {
    if (fchmod(fd, 0600) < 0)
      throw std::runtime_error("cannot secure VST3 state file");
    writeAll(fd, bytes.data(), bytes.size());
    if (fsync(fd) < 0)
      throw std::runtime_error("cannot sync VST3 state file");
    const int closeResult = close(fd);
    fdOpen = false;
    if (closeResult < 0)
      throw std::runtime_error("cannot close VST3 state file");
    if (rename(temp.data(), path.c_str()) < 0)
      throw std::runtime_error("cannot atomically replace VST3 state file");
    renamed = true;
    const int directoryFd =
        open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directoryFd < 0)
      throw std::runtime_error("cannot open VST3 state directory for sync");
    if (fsync(directoryFd) < 0) {
      close(directoryFd);
      throw std::runtime_error("cannot sync VST3 state directory");
    }
    close(directoryFd);
  } catch (...) {
    if (fdOpen)
      close(fd);
    if (!renamed)
      unlink(temp.data());
    throw;
  }
}
static_assert(std::atomic<bool>::is_always_lock_free,
              "VST3 failure latch must be lock-free on the audio thread");
static_assert(std::atomic<float>::is_always_lock_free,
              "VST3 parameter values must be lock-free on the audio thread");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "VST3 parameter revisions must be lock-free on the audio thread");
struct CatalogItem {
  Module::Ptr module;
  VST3::Hosting::ClassInfo info;
  std::string uid;
  std::string name;
};
using Catalog = std::vector<CatalogItem>;
Steinberg::Vst::ParamID parseParameterId(const std::string &symbol) {
  if (symbol.empty() ||
      !std::all_of(symbol.begin(), symbol.end(),
                   [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
    throw std::runtime_error("VST3 parameter IDs must be decimal numbers");
  size_t consumed = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(symbol, &consumed);
  } catch (const std::exception &) {
    throw std::runtime_error("VST3 parameter ID is out of range: " + symbol);
  }
  if (consumed != symbol.size() ||
      value > std::numeric_limits<Steinberg::Vst::ParamID>::max())
    throw std::runtime_error("VST3 parameter ID is out of range: " + symbol);
  return static_cast<Steinberg::Vst::ParamID>(value);
}

Steinberg::Vst::HostApplication &hostApplication() {
  static Steinberg::Vst::HostApplication host;
  return host;
}

std::string parameterName(const Steinberg::Vst::ParameterInfo &info) {
  std::wstring wide;
  for (const auto ch : info.title) {
    if (!ch)
      break;
    wide.push_back(static_cast<wchar_t>(ch));
  }
  return StringHelper::toString(wide, 65001);
}

std::vector<PluginParameterInfo>
readParameters(const Module::Ptr &module,
               const VST3::Hosting::ClassInfo &classInfo) {
  using namespace Steinberg;
  using namespace Steinberg::Vst;
  PluginContextFactory::instance().setPluginContext(&hostApplication());
  PlugProvider provider(module->getFactory(), classInfo);
  if (!provider.initialize())
    throw std::runtime_error("VST3 plugin initialization failed: " +
                             classInfo.ID().toString(false));
  auto controller = provider.getControllerPtr();
  std::vector<PluginParameterInfo> result;
  if (!controller)
    return result;
  const int32 count = controller->getParameterCount();
  if (count < 0 || count > 4096)
    throw std::runtime_error("VST3 plugin declares too many parameters");
  result.reserve(static_cast<size_t>(count));
  for (int32 index = 0; index < count; ++index) {
    ParameterInfo info{};
    if (controller->getParameterInfo(index, info) != kResultOk ||
        (info.flags & ParameterInfo::kIsReadOnly))
      continue;
    const auto value = controller->getParamNormalized(info.id);
    result.push_back({std::to_string(info.id), parameterName(info),
                      static_cast<float>(info.defaultNormalizedValue), 0.0f,
                      1.0f, static_cast<float>(value)});
  }
  return result;
}

std::vector<std::string> modulePaths() {
  // Useful for hermetic tests and deployments that intentionally want one
  // private catalog. Normal operation continues to include SDK system paths.
  const char *pathsOnly = std::getenv("SKYAPO_VST3_PATHS_ONLY");
  auto paths = pathsOnly && std::string_view(pathsOnly) == "1"
                   ? std::vector<std::string>{}
                   : Module::getModulePaths();
  if (const char *env = std::getenv("VST3_PATH")) {
    std::string list(env);
    size_t begin = 0;
    while (begin <= list.size()) {
      const size_t end = list.find(':', begin);
      const auto value = list.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin);
      if (!value.empty()) {
        std::error_code ec;
        const std::filesystem::path root(value);
        if (root.extension() == ".vst3") {
          paths.push_back(root.string());
        } else if (std::filesystem::is_directory(root, ec)) {
          std::filesystem::recursive_directory_iterator it(
              root, std::filesystem::directory_options::skip_permission_denied,
              ec),
              finish;
          for (; it != finish; it.increment(ec)) {
            if (ec) {
              ec.clear();
              continue;
            }
            if (it->is_directory(ec) && it->path().extension() == ".vst3") {
              paths.push_back(it->path().string());
              it.disable_recursion_pending();
            }
          }
        }
      }
      if (end == std::string::npos)
        break;
      begin = end + 1;
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

const Catalog &catalog() {
  static const Catalog cache = [] {
    Catalog result;
    for (const auto &path : modulePaths()) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
        continue;
      std::string error;
      auto module = Module::create(path, error);
      if (!module) {
        if (std::getenv("SKYAPO_VST3_DEBUG"))
          std::cerr << "VST3: unable to load '" << path << "': " << error
                    << '\n';
        continue;
      }
      for (const auto &info : module->getFactory().classInfos()) {
        if (info.category() == "Audio Module Class")
          result.push_back(
              {module, info, info.ID().toString(false), info.name()});
        else if (std::getenv("SKYAPO_VST3_DEBUG"))
          std::cerr << "VST3: skip class '" << info.name() << "' category '"
                    << info.category() << "' in '" << path << "'\n";
      }
    }
    return result;
  }();
  return cache;
}

class VST3Instance final : public IPluginInstance,
                           public IPluginParameterControl,
                           public IPluginStatePersistence {
public:
  struct LiveParameter {
    std::atomic<float> value{0.0f};
    std::atomic<uint64_t> revision{0};
    uint64_t consumedRevision{};
  };

  VST3Instance(Module::Ptr pluginModule,
               const VST3::Hosting::ClassInfo &classInfo, std::string uid,
               float sampleRate, unsigned maxFrames,
               const std::vector<std::wstring> &channels,
               const std::vector<PluginParameterValue> &overrides,
               std::string identity = {})
      : module(std::move(pluginModule)), pluginUid(std::move(uid)),
        channelCount(channels.size()), maxFrameCount(maxFrames),
        persistentIdentity(std::move(identity)) {
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || !maxFrames ||
        (channelCount != 1 && channelCount != 2))
      throw std::runtime_error("VST3 host currently supports mono/stereo only");
    PluginContextFactory::instance().setPluginContext(&hostApplication());
    provider = std::make_unique<PlugProvider>(module->getFactory(), classInfo);
    if (!provider->initialize())
      throw std::runtime_error("VST3 plugin initialization failed: " +
                               pluginUid);
    component = provider->getComponentPtr();
    processor = FUnknownPtr<IAudioProcessor>(component);
    if (!component || !processor)
      throw std::runtime_error("VST3 class lacks IComponent/IAudioProcessor: " +
                               pluginUid);
    parameterController = provider->getControllerPtr();
    std::optional<VST3StateData> savedState;
    if (!persistentIdentity.empty()) {
      savedStatePath = statePathFor(persistentIdentity);
      savedState = readStateFile(savedStatePath, persistentIdentity);
    }
    if (parameterController) {
      const int32 count = parameterController->getParameterCount();
      if (count < 0 || count > 4096)
        throw std::runtime_error("VST3 plugin declares too many parameters: " +
                                 pluginUid);
      parameterInfos.reserve(static_cast<size_t>(count));
      for (int32 index = 0; index < count; ++index) {
        ParameterInfo info{};
        if (parameterController->getParameterInfo(index, info) != kResultOk ||
            (info.flags & ParameterInfo::kIsReadOnly))
          continue;
        parameterInfos.push_back(
            {std::to_string(info.id), parameterName(info),
             static_cast<float>(info.defaultNormalizedValue), 0.0f, 1.0f,
             static_cast<float>(
                 parameterController->getParamNormalized(info.id))});
        parameterIds.push_back(info.id);
      }
    }
    for (const auto &overrideValue : overrides) {
      const auto found =
          std::find_if(parameterInfos.begin(), parameterInfos.end(),
                       [&](const auto &parameter) {
                         return parameter.symbol == overrideValue.symbol;
                       });
      if (found == parameterInfos.end())
        throw std::runtime_error("unknown or read-only VST3 parameter '" +
                                 overrideValue.symbol + "' in " + pluginUid);
      if (!std::isfinite(overrideValue.value) || overrideValue.value < 0.0f ||
          overrideValue.value > 1.0f)
        throw std::runtime_error("VST3 parameter '" + overrideValue.symbol +
                                 "' must be in normalized range [0, 1]");
      if (!parameterController || parameterController->setParamNormalized(
                                      parseParameterId(overrideValue.symbol),
                                      overrideValue.value) != kResultOk)
        throw std::runtime_error("VST3 plugin rejected parameter '" +
                                 overrideValue.symbol + "'");
      found->value = overrideValue.value;
    }
    // Match CLAP/LV2 behavior: persisted interactive state is restored after
    // config-time initialization overrides, so a restart preserves the last
    // live value rather than silently reverting it to the directive default.
    if (savedState)
      restoreState(*savedState);
    liveParameters =
        parameterInfos.empty()
            ? nullptr
            : std::make_unique<LiveParameter[]>(parameterInfos.size());
    for (size_t i = 0; i < parameterInfos.size(); ++i)
      liveParameters[i].value.store(parameterInfos[i].value,
                                    std::memory_order_relaxed);
    if (!savedState)
      for (const auto &overrideValue : overrides) {
        const auto found =
            std::find_if(parameterInfos.begin(), parameterInfos.end(),
                         [&](const auto &parameter) {
                           return parameter.symbol == overrideValue.symbol;
                         });
        const size_t index =
            static_cast<size_t>(found - parameterInfos.begin());
        liveParameters[index].revision.store(1, std::memory_order_relaxed);
      }
    // ParameterChanges / ParameterValueQueue are SDK convenience classes, not
    // thread-safe, and their vectors may allocate. They are exclusively owned
    // by the audio thread once processing starts. Size every queue and prime
    // its point vector here, before activation, so one automation point per
    // parameter can be submitted without growing either vector in process().
    parameterChanges = std::make_unique<ParameterChanges>(
        static_cast<int32>(parameterInfos.size()));
    for (size_t i = 0; i < parameterInfos.size(); ++i) {
      Steinberg::int32 queueIndex = 0;
      auto *queue =
          parameterChanges->addParameterData(parameterIds[i], queueIndex);
      Steinberg::int32 pointIndex = 0;
      if (!queue || queue->addPoint(0, parameterInfos[i].value, pointIndex) !=
                        kResultTrue)
        throw std::runtime_error("cannot prepare VST3 parameter queue");
    }
    parameterChanges->clearQueue();
    if (component->getBusCount(kAudio, kInput) != 1 ||
        component->getBusCount(kAudio, kOutput) != 1)
      throw std::runtime_error(
          "VST3 plugin must have one audio input/output bus: " + pluginUid);
    BusInfo inputInfo{}, outputInfo{};
    if (component->getBusInfo(kAudio, kInput, 0, inputInfo) != kResultOk ||
        component->getBusInfo(kAudio, kOutput, 0, outputInfo) != kResultOk)
      throw std::runtime_error(
          "VST3 plugin audio bus layout does not match host: " + pluginUid);
    if (inputInfo.channelCount != static_cast<int32>(channelCount) ||
        outputInfo.channelCount != static_cast<int32>(channelCount))
      throw std::runtime_error("VST3 plugin bus channels are " +
                               std::to_string(inputInfo.channelCount) + "/" +
                               std::to_string(outputInfo.channelCount) +
                               ", host requires " +
                               std::to_string(channelCount) + "/" +
                               std::to_string(channelCount) + ": " + pluginUid);
    SpeakerArrangement arrangement =
        channelCount == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
    if (processor->setBusArrangements(&arrangement, 1, &arrangement, 1) !=
        kResultOk)
      throw std::runtime_error("VST3 plugin rejected mono/stereo layout: " +
                               pluginUid);
    if (component->activateBus(kAudio, kInput, 0, true) != kResultOk ||
        component->activateBus(kAudio, kOutput, 0, true) != kResultOk)
      throw std::runtime_error("VST3 plugin bus activation failed: " +
                               pluginUid);
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(maxFrames);
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk ||
        component->setActive(true) != kResultOk)
      throw std::runtime_error("VST3 plugin setup failed: " + pluginUid);
    active = true;
    latency = processor->getLatencySamples();
    // Steinberg's own AudioClient does not gate activation on this return
    // value: several compliant plug-ins return kResultFalse while still
    // processing normally. We still check the process() result per block.
    (void)processor->setProcessing(true);
    processing = true;
    inputs = std::make_unique<float *[]>(channelCount);
    outputs = std::make_unique<float *[]>(channelCount);
    inputBus.numChannels = outputBus.numChannels =
        static_cast<int32>(channelCount);
    inputBus.channelBuffers32 = inputs.get();
    outputBus.channelBuffers32 = outputs.get();
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numInputs = data.numOutputs = 1;
    data.inputs = &inputBus;
    data.outputs = &outputBus;
    data.inputParameterChanges = nullptr;
  }

  ~VST3Instance() override {
    if (processing)
      processor->setProcessing(false);
    if (active)
      component->setActive(false);
  }

  const std::string &uri() const noexcept override {
    return pluginUid;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return pluginUid;
  }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfos;
  }
  uint32_t latencySamples() const noexcept override {
    return latency;
  }
  std::vector<std::wstring>
  initialize(float, unsigned,
             const std::vector<std::wstring> &channels) override {
    return channels;
  }
  void process(float **output, float **input,
               unsigned frames) noexcept override {
    if (frames > maxFrameCount ||
        processingError.load(std::memory_order_acquire)) {
      for (unsigned c = 0; c < channelCount; ++c)
        std::fill_n(output[c], frames, 0.0f);
      return;
    }
    for (size_t c = 0; c < channelCount; ++c) {
      inputs[c] = input[c];
      outputs[c] = output[c];
    }
    data.numSamples = static_cast<int32_t>(frames);
    buildParameterChanges();
    if (processor->process(data) != Steinberg::kResultOk) {
      processingError.store(true, std::memory_order_release);
      for (size_t c = 0; c < channelCount; ++c)
        std::fill_n(output[c], frames, 0.0f);
    }
  }

  bool processingFailed() const noexcept override {
    return processingError.load(std::memory_order_acquire);
  }

  void setParameterValue(const std::string &symbol, float value) override {
    const auto id = parseParameterId(symbol);
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
      throw std::runtime_error("VST3 parameter '" + symbol +
                               "' must be in normalized range [0, 1]");
    const auto found = std::find(parameterIds.begin(), parameterIds.end(), id);
    if (found == parameterIds.end())
      throw std::runtime_error("unknown or read-only VST3 parameter '" +
                               symbol + "' in " + pluginUid);
    const size_t index = static_cast<size_t>(found - parameterIds.begin());
    // The serialized control thread is the sole writer. The audio thread reads
    // the published value after acquiring the revision and emits it as a VST3
    // automation point; controller calls are deliberately excluded here.
    liveParameters[index].value.store(value, std::memory_order_relaxed);
    liveParameters[index].revision.fetch_add(1, std::memory_order_release);
  }

  bool savePersistentPluginState() override {
    if (persistentIdentity.empty())
      return false;
    if (processingError.load(std::memory_order_acquire))
      throw std::runtime_error("cannot persist failed VST3 plugin state: " +
                               pluginUid);
    VST3StateData state;
    // The caller has quiesced the audio callback. Synchronize the latest live
    // parameter mailbox values before asking the VST3 component/controller to
    // snapshot their state, including an update made just before shutdown.
    for (size_t i = 0; i < parameterInfos.size(); ++i) {
      if (!liveParameters[i].revision.load(std::memory_order_acquire))
        continue;
      const auto value =
          liveParameters[i].value.load(std::memory_order_acquire);
      if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        throw std::runtime_error(
            "invalid VST3 parameter value while saving state");
      if (parameterController &&
          parameterController->setParamNormalized(parameterIds[i], value) !=
              Steinberg::kResultOk)
        throw std::runtime_error("VST3 controller rejected state parameter: " +
                                 pluginUid);
    }
    auto saveStream = [](auto *object, auto method, std::string &destination,
                         const char *description) {
      if (!object)
        return;
      Steinberg::MemoryStream stream;
      const auto result = (object->*method)(&stream);
      if (result == Steinberg::kNotImplemented)
        return;
      if (result != Steinberg::kResultOk)
        throw std::runtime_error(std::string("VST3 plugin failed to save ") +
                                 description + " state");
      const auto size = stream.getSize();
      if (size < 0 || static_cast<uint64_t>(size) > MaxVST3StateBytes)
        throw std::runtime_error("VST3 plugin state exceeds the 16 MiB limit");
      if (size)
        destination.assign(stream.getData(), static_cast<size_t>(size));
    };
    saveStream(component.get(), &Steinberg::Vst::IComponent::getState,
               state.component, "component");
    saveStream(parameterController.get(),
               &Steinberg::Vst::IEditController::getState, state.controller,
               "controller");
    if (state.component.empty() && state.controller.empty())
      return false;
    atomicWriteState(savedStatePath, persistentIdentity, state);
    return true;
  }

private:
  void restoreState(const VST3StateData &state) {
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if (!state.component.empty()) {
      MemoryStream stream(const_cast<char *>(state.component.data()),
                          static_cast<TSize>(state.component.size()));
      if (component->setState(&stream) != kResultOk)
        throw std::runtime_error("VST3 component rejected saved state: " +
                                 pluginUid);
      if (parameterController) {
        stream.seek(0, IBStream::kIBSeekSet, nullptr);
        const auto result = parameterController->setComponentState(&stream);
        if (result != kResultOk && result != kNotImplemented)
          throw std::runtime_error(
              "VST3 controller rejected component state: " + pluginUid);
      }
    }
    if (!state.controller.empty()) {
      if (!parameterController)
        throw std::runtime_error(
            "VST3 state contains controller data but no controller: " +
            pluginUid);
      MemoryStream stream(const_cast<char *>(state.controller.data()),
                          static_cast<TSize>(state.controller.size()));
      if (parameterController->setState(&stream) != kResultOk)
        throw std::runtime_error("VST3 controller rejected saved state: " +
                                 pluginUid);
    }
  }

  void buildParameterChanges() noexcept {
    parameterChanges->clearQueue();
    bool hasChanges = false;
    for (size_t i = 0; i < parameterInfos.size(); ++i) {
      const uint64_t revision =
          liveParameters[i].revision.load(std::memory_order_acquire);
      if (!revision || revision == liveParameters[i].consumedRevision)
        continue;
      Steinberg::int32 queueIndex = 0;
      auto *queue =
          parameterChanges->addParameterData(parameterIds[i], queueIndex);
      if (!queue)
        continue; // Capacity was reserved for every writable parameter.
      Steinberg::int32 pointIndex = 0;
      const double value =
          liveParameters[i].value.load(std::memory_order_relaxed);
      if (queue->addPoint(0, value, pointIndex) != Steinberg::kResultTrue)
        continue; // Each queue was primed and has reserved point capacity.
      liveParameters[i].consumedRevision = revision;
      hasChanges = true;
    }
    data.inputParameterChanges = hasChanges ? parameterChanges.get() : nullptr;
  }

  Module::Ptr module;
  std::string pluginUid;
  size_t channelCount{};
  unsigned maxFrameCount{};
  uint32_t latency{};
  std::unique_ptr<Steinberg::Vst::PlugProvider> provider;
  Steinberg::IPtr<Steinberg::Vst::IComponent> component;
  Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> processor;
  std::unique_ptr<float *[]> inputs, outputs;
  Steinberg::Vst::AudioBusBuffers inputBus{}, outputBus{};
  Steinberg::Vst::ProcessData data{};
  Steinberg::IPtr<Steinberg::Vst::IEditController> parameterController;
  std::vector<PluginParameterInfo> parameterInfos;
  std::vector<Steinberg::Vst::ParamID> parameterIds;
  std::unique_ptr<LiveParameter[]> liveParameters;
  std::unique_ptr<Steinberg::Vst::ParameterChanges> parameterChanges;
  std::string persistentIdentity;
  fs::path savedStatePath;
  bool active = false, processing = false;
  std::atomic<bool> processingError{false};
};

class VST3PluginFilter final : public IFilter,
                               public AtomicPluginBypass,
                               public IPluginParameterControl,
                               public IPluginFailureState,
                               public IPluginLatencyState,
                               public IPluginStatePersistence {
public:
  VST3PluginFilter(VST3PluginHost &owner, std::string uid,
                   std::vector<PluginParameterValue> overrides,
                   fs::path source = {}, unsigned line = 0)
      : host(owner), pluginUid(std::move(uid)),
        parameterOverrides(std::move(overrides)), source(std::move(source)),
        line(line) {}
  bool getInPlace() override {
    return false;
  }
  std::vector<std::wstring>
  initialize(float rate, unsigned maxFrames,
             std::vector<std::wstring> channels) override {
    channelCount = static_cast<unsigned>(channels.size());
    instance = source.empty()
                   ? host.create(pluginUid, rate, maxFrames, channels,
                                 parameterOverrides)
                   : host.createForConfig(pluginUid, rate, maxFrames, channels,
                                          parameterOverrides, source, line);
    auto outputChannels = instance->initialize(rate, maxFrames, channels);
    prepareBypassDelay(instance->latencySamples(), channelCount);
    return outputChannels;
  }
  void process(float **output, float **input, unsigned frames) override {
    if (copyInputWhenBypassed(output, input, frames, channelCount))
      return;
    instance->process(output, input, frames);
  }
  bool processingFailed() const noexcept override {
    return instance && instance->processingFailed();
  }
  const std::string &failureIdentifier() const noexcept override {
    return pluginUid;
  }
  uint32_t latencySamples() const noexcept override {
    return instance ? instance->latencySamples() : 0;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return pluginUid;
  }
  void setParameterValue(const std::string &symbol, float value) override {
    auto *control = dynamic_cast<IPluginParameterControl *>(instance.get());
    if (!control)
      throw std::runtime_error("VST3 plugin is not active: " + pluginUid);
    control->setParameterValue(symbol, value);
  }
  bool savePersistentPluginState() override {
    auto *state = dynamic_cast<IPluginStatePersistence *>(instance.get());
    return state && state->savePersistentPluginState();
  }

private:
  VST3PluginHost &host;
  std::string pluginUid;
  std::vector<PluginParameterValue> parameterOverrides;
  fs::path source;
  unsigned line{};
  std::unique_ptr<IPluginInstance> instance;
  unsigned channelCount{};
};

IFilter *allocateFilter(VST3PluginHost &host, std::string uid,
                        std::vector<PluginParameterValue> overrides,
                        fs::path source = {}, unsigned line = 0) {
  void *memory = MemoryHelper::alloc(sizeof(VST3PluginFilter));
  try {
    return new (memory) VST3PluginFilter(
        host, std::move(uid), std::move(overrides), std::move(source), line);
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

class VST3FilterFactory final : public IFilterFactory,
                                public IPluginSourceContext {
public:
  VST3FilterFactory() : host(std::make_unique<VST3PluginHost>()) {}
  void setPluginSourceLocation(const fs::path &path,
                               unsigned sourceLine) override {
    source = path;
    line = sourceLine;
  }
  std::vector<IFilter *> createFilter(const std::wstring &,
                                      std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, uid;
    input >> format >> uid;
    if (format != L"VST3")
      return {};
    if (uid.empty())
      throw std::runtime_error(
          "expected Plugin: VST3 <class-uid> [parameter-id=value ...]");
    std::vector<PluginParameterValue> overrides;
    std::wstring token;
    while (input >> token) {
      const auto separator = token.find(L'=');
      if (separator == std::wstring::npos || separator == 0 ||
          separator + 1 == token.size())
        throw std::runtime_error(
            "expected VST3 parameter as parameter-id=value");
      const auto symbol =
          StringHelper::toString(token.substr(0, separator), 65001);
      try {
        size_t consumed = 0;
        const auto valueText =
            StringHelper::toString(token.substr(separator + 1), 65001);
        const float value = std::stof(valueText, &consumed);
        if (consumed != valueText.size() || !std::isfinite(value))
          throw std::runtime_error("not finite");
        if (std::any_of(
                overrides.begin(), overrides.end(),
                [&](const auto &entry) { return entry.symbol == symbol; }))
          throw std::runtime_error("duplicate");
        overrides.push_back({symbol, value});
      } catch (const std::exception &) {
        throw std::runtime_error(
            "invalid or duplicate VST3 parameter override '" + symbol + "'");
      }
    }
    return {allocateFilter(*host, StringHelper::toString(uid, 65001),
                           std::move(overrides), source, line)};
  }

private:
  std::unique_ptr<VST3PluginHost> host;
  fs::path source;
  unsigned line{};
};
} // namespace

std::unique_ptr<IPluginInstance>
VST3PluginHost::create(const std::string &uid, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  return createForConfig(uid, sampleRate, maxFrames, channels, parameters, {},
                         0);
}

std::unique_ptr<IPluginInstance> VST3PluginHost::createForConfig(
    const std::string &uid, float sampleRate, unsigned maxFrames,
    const std::vector<std::wstring> &channels,
    const std::vector<PluginParameterValue> &parameters, const fs::path &source,
    unsigned line) {
  if (!parameters.empty())
    for (const auto &parameter : parameters)
      (void)parseParameterId(parameter.symbol);
  for (const auto &item : catalog())
    if (item.uid == uid) {
      const std::string identity =
          source.empty() ? std::string{}
                         : fs::absolute(source).lexically_normal().string() +
                               "\n" + std::to_string(line) + "\n" + uid;
      return std::make_unique<VST3Instance>(item.module, item.info, uid,
                                            sampleRate, maxFrames, channels,
                                            parameters, identity);
    }
  throw std::runtime_error("VST3 class UID not found: " + uid);
}

PluginDescription VST3PluginHost::describe(const std::string &uid) const {
  for (const auto &item : catalog())
    if (item.uid == uid)
      return {item.uid, item.name, readParameters(item.module, item.info)};
  throw std::runtime_error("VST3 class UID not found: " + uid);
}

std::vector<std::pair<std::string, std::string>> VST3PluginHost::list() const {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &item : catalog())
    result.emplace_back(item.uid, item.name);
  return result;
}

std::unique_ptr<IFilterFactory> makeVST3PluginFilterFactory() {
  return std::make_unique<VST3FilterFactory>();
}
