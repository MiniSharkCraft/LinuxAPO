#include "VST2PluginHost.h"

#include "fst.h"
#include "IPluginParameterControl.h"
#include "IPluginLatencyRefresh.h"
#include "PluginLatencyLimits.h"
#include "IPluginStatePersistence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
// FST exposes this VST2 ABI opcode but marks its enum entry as unknown. The
// host handles it for plugins that use the standard VST2 I/O-change signal.
constexpr int Vst2AudioMasterIOChangedOpcode = audioMasterIOChanged;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static_assert(
    std::atomic<bool>::is_always_lock_free,
    "VST2 prototype failure latch must be lock-free on the audio thread");
static_assert(std::atomic<size_t>::is_always_lock_free,
              "VST2 parameter queue indices must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<float>::is_always_lock_free,
              "VST2 parameter mailboxes must be lock-free");

struct HostContext {
  float sampleRate{};
  unsigned blockSize{};
  std::atomic<bool> *latencyChanged{};
};

thread_local HostContext *constructingContext = nullptr;

t_fstPtrInt audioMaster(AEffect *effect, int opcode, int, t_fstPtrInt, void *,
                        float) {
  if (opcode == audioMasterVersion)
    return kVstVersion;
  if (opcode == audioMasterGetCurrentProcessLevel)
    return kVstProcessLevelRealtime;
  // A plugin is allowed to call audioMaster during VSTPluginMain before the
  // host has received its AEffect and assigned effect->user. Prefer the
  // current construction context in that window; afterward, use the per-effect
  // context for concurrent plugin instances.
  HostContext *context = constructingContext;
  if (!context && effect)
    context = static_cast<HostContext *>(effect->user);
  switch (opcode) {
  case audioMasterGetSampleRate:
    return context ? static_cast<t_fstPtrInt>(context->sampleRate) : 0;
  case audioMasterGetBlockSize:
    return context ? static_cast<t_fstPtrInt>(context->blockSize) : 0;
  case Vst2AudioMasterIOChangedOpcode:
    // VST2 sends this from the audio thread when its I/O/latency properties
    // change. Only latch a lock-free notification here; inspection and PDC
    // rebuilding happen after the host has quiesced audio processing.
    if (context && context->latencyChanged)
      context->latencyChanged->store(true, std::memory_order_release);
    return 1;
  default:
    return 0;
  }
}

using PluginMain = AEffect *(*)(audioMasterCallback);
namespace fs = std::filesystem;
constexpr size_t MaxVST2StateBytes = 16 * 1024 * 1024;
constexpr std::array<uint8_t, 8> VST2StateMagic{'S', 'K', 'Y', 'V', 'S',
                                                'T', '2', '1'};

uint64_t stateHash(std::string_view text) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t stateChecksum(std::string_view identity,
                       const std::vector<uint8_t> &payload) {
  uint64_t hash = stateHash(identity);
  hash ^= 0xff;
  hash *= 1099511628211ull;
  for (const auto byte : payload) {
    hash ^= byte;
    hash *= 1099511628211ull;
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
    throw std::runtime_error("VST2 state identity exceeds 4096 bytes");
  fs::path base;
  if (const char *xdg = std::getenv("XDG_STATE_HOME");
      xdg && *xdg && fs::path(xdg).is_absolute())
    base = xdg;
  else if (const char *home = std::getenv("HOME"); home && *home)
    base = fs::path(home) / ".local" / "state";
  else
    throw std::runtime_error("VST2 state needs XDG_STATE_HOME or HOME");
  std::ostringstream name;
  name << std::hex << std::setw(16) << std::setfill('0') << stateHash(identity)
       << ".vst2state";
  return base / "skyapo" / "vst2-state" / name.str();
}

std::vector<uint8_t> encodeState(std::string_view identity,
                                 const std::vector<uint8_t> &payload) {
  if (identity.size() > 4096 || payload.empty() ||
      payload.size() > MaxVST2StateBytes)
    throw std::runtime_error("VST2 plugin state exceeds the 16 MiB limit");
  std::vector<uint8_t> bytes;
  bytes.reserve(VST2StateMagic.size() + 4 + 8 * 3 + identity.size() +
                payload.size());
  bytes.insert(bytes.end(), VST2StateMagic.begin(), VST2StateMagic.end());
  appendU32(bytes, 1);
  appendU64(bytes, identity.size());
  appendU64(bytes, payload.size());
  appendU64(bytes, stateChecksum(identity, payload));
  bytes.insert(bytes.end(), identity.begin(), identity.end());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

std::vector<uint8_t> decodeState(const std::vector<uint8_t> &bytes,
                                 std::string_view expectedIdentity) {
  size_t offset = VST2StateMagic.size();
  uint32_t version{};
  uint64_t identitySize{}, payloadSize{}, checksum{};
  if (bytes.size() < VST2StateMagic.size() ||
      !std::equal(VST2StateMagic.begin(), VST2StateMagic.end(), bytes.begin()) ||
      !takeU32(bytes, offset, version) || version != 1 ||
      !takeU64(bytes, offset, identitySize) || identitySize > 4096 ||
      !takeU64(bytes, offset, payloadSize) || !payloadSize ||
      payloadSize > MaxVST2StateBytes || !takeU64(bytes, offset, checksum) ||
      identitySize > bytes.size() - offset ||
      payloadSize != bytes.size() - offset - identitySize)
    throw std::runtime_error("corrupt VST2 state sidecar");
  const std::string identity(
      reinterpret_cast<const char *>(bytes.data() + offset),
      static_cast<size_t>(identitySize));
  offset += static_cast<size_t>(identitySize);
  if (identity != expectedIdentity)
    throw std::runtime_error("VST2 state sidecar identity mismatch");
  std::vector<uint8_t> payload(bytes.begin() + offset, bytes.end());
  if (checksum != stateChecksum(identity, payload))
    throw std::runtime_error("VST2 state sidecar checksum mismatch");
  return payload;
}

struct ScopedFd {
  int fd{-1};
  explicit ScopedFd(int value) : fd(value) {}
  ~ScopedFd() {
    if (fd >= 0)
      close(fd);
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
};

std::optional<std::vector<uint8_t>> readStateFile(const fs::path &path,
                                                  std::string_view identity) {
  const int rawFd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (rawFd < 0) {
    if (errno == ENOENT)
      return std::nullopt;
    throw std::runtime_error("cannot open VST2 state sidecar: " +
                             std::string(std::strerror(errno)));
  }
  ScopedFd fd(rawFd);
  struct stat status {};
  constexpr size_t HeaderSize = 8 + 4 + 8 * 3;
  if (fstat(rawFd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0 ||
      status.st_size < static_cast<off_t>(HeaderSize) ||
      static_cast<uint64_t>(status.st_size) > MaxVST2StateBytes + 8192)
    throw std::runtime_error("invalid or oversized VST2 state sidecar");
  std::vector<uint8_t> bytes(static_cast<size_t>(status.st_size));
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = read(rawFd, bytes.data() + offset,
                               bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("short read from VST2 state sidecar");
    offset += static_cast<size_t>(count);
  }
  uint8_t extra{};
  ssize_t count;
  do {
    count = read(rawFd, &extra, 1);
  } while (count < 0 && errno == EINTR);
  if (count != 0)
    throw std::runtime_error("VST2 state sidecar changed while being read");
  return decodeState(bytes, identity);
}

void writeAll(int fd, const uint8_t *bytes, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const ssize_t count = write(fd, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot write VST2 state sidecar");
    offset += static_cast<size_t>(count);
  }
}

void atomicWriteState(const fs::path &path, std::string_view identity,
                      const std::vector<uint8_t> &payload) {
  const auto bytes = encodeState(identity, payload);
  const auto directory = path.parent_path();
  std::error_code error;
  fs::create_directories(directory, error);
  if (error || fs::is_symlink(fs::symlink_status(directory, error)) || error)
    throw std::runtime_error("cannot safely create VST2 state directory");
  if (chmod(directory.c_str(), 0700) < 0)
    throw std::runtime_error("cannot secure VST2 state directory");
  std::string pattern = path.string() + ".tmp-XXXXXX";
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const int rawFd = mkstemp(temporary.data());
  if (rawFd < 0)
    throw std::runtime_error("cannot create VST2 state temporary file");
  ScopedFd fd(rawFd);
  bool renamed = false;
  try {
    if (fchmod(rawFd, 0600) < 0)
      throw std::runtime_error("cannot secure VST2 state sidecar");
    writeAll(rawFd, bytes.data(), bytes.size());
    if (fsync(rawFd) < 0)
      throw std::runtime_error("cannot sync VST2 state sidecar");
    if (close(rawFd) < 0)
      throw std::runtime_error("cannot close VST2 state sidecar");
    fd.fd = -1;
    if (rename(temporary.data(), path.c_str()) < 0)
      throw std::runtime_error("cannot atomically replace VST2 state sidecar");
    renamed = true;
    const int directoryFd =
        open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directoryFd < 0)
      throw std::runtime_error("cannot open VST2 state directory for sync");
    ScopedFd directoryOwner(directoryFd);
    if (fsync(directoryFd) < 0)
      throw std::runtime_error("cannot sync VST2 state directory");
  } catch (...) {
    if (!renamed)
      unlink(temporary.data());
    throw;
  }
}

class DynamicModule {
public:
  explicit DynamicModule(const std::string &path) {
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      const char *error = dlerror();
      throw std::runtime_error("VST2: cannot load '" + path + "': " +
                               (error ? error : "unknown loader error"));
    }
  }
  ~DynamicModule() {
    if (handle)
      dlclose(handle);
  }
  DynamicModule(const DynamicModule &) = delete;
  DynamicModule &operator=(const DynamicModule &) = delete;

  PluginMain entryPoint() const {
    dlerror();
    auto *entry = reinterpret_cast<PluginMain>(dlsym(handle, "VSTPluginMain"));
    if (!entry) {
      dlerror();
      entry = reinterpret_cast<PluginMain>(dlsym(handle, "main"));
    }
    if (!entry) {
      const char *error = dlerror();
      throw std::runtime_error("VST2: module has no VSTPluginMain/main entry "
                               "point" +
                               (error ? std::string(": ") + error : ""));
    }
    return entry;
  }

private:
  void *handle{};
};

std::string parameterName(AEffect *effect, int index) {
  char name[kVstMaxParamStrLen + 1]{};
  if (effect->dispatcher)
    effect->dispatcher(effect, effGetParamName, index, 0, name, 0.0f);
  name[kVstMaxParamStrLen] = '\0';
  if (name[0])
    return name;
  return "Parameter " + std::to_string(index);
}

class VST2Instance final : public IPluginInstance,
                           public IPluginParameterControl,
                           public IPluginStatePersistence,
                           public IPluginLatencyRefresh {
public:
  VST2Instance(std::string path, float rate, unsigned maxFrames,
               const std::vector<std::wstring> &channels,
               const std::vector<PluginParameterValue> &overrides,
               const fs::path &source, unsigned sourceLine)
      : modulePath(std::move(path)), context{rate, maxFrames},
        channelCount(channels.size()), maxFrameCount(maxFrames) {
    context.latencyChanged = &latencyChanged;
    if (!std::isfinite(rate) || rate < 8000.0f || rate > 384000.0f ||
        maxFrames == 0 || (channelCount != 1 && channelCount != 2))
      throw std::runtime_error("VST2 prototype supports mono/stereo and valid "
                               "sample rate/block size only");

    module = std::make_unique<DynamicModule>(modulePath);
    struct ContextGuard {
      explicit ContextGuard(HostContext *value)
          : previous(constructingContext) {
        constructingContext = value;
      }
      ~ContextGuard() {
        constructingContext = previous;
      }
      HostContext *previous;
    } guard(&context);
    effect = module->entryPoint()(&audioMaster);
    if (!effect)
      throw std::runtime_error("VST2: plugin entry point returned null");
    if (effect->magic != kEffectMagic) {
      effect = nullptr;
      throw std::runtime_error("VST2: invalid AEffect magic");
    }
    effect->user = &context;
    try {
      if (!effect->dispatcher || !effect->processReplacing ||
          effect->numInputs != static_cast<t_fstInt32>(channelCount) ||
          effect->numOutputs != static_cast<t_fstInt32>(channelCount))
        throw std::runtime_error(
            "VST2: plugin must expose matching mono/stereo inputs and outputs "
            "with processReplacing");

      effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
      opened = true;
      effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, rate);
      effect->dispatcher(effect, effSetBlockSize, 0,
                         static_cast<t_fstPtrInt>(maxFrames), nullptr, 0.0f);

      if (effect->numParams < 0 || effect->numParams > 4096)
        throw std::runtime_error("VST2: unreasonable parameter count");
      parameterCount = static_cast<size_t>(effect->numParams);
      if (parameterCount)
        pendingValues = std::make_unique<std::atomic<float>[]>(parameterCount);
      for (auto &word : pendingMask)
        word.store(0, std::memory_order_relaxed);
      parameterInfos.reserve(static_cast<size_t>(effect->numParams));
      for (int index = 0; index < effect->numParams; ++index) {
        const float value =
            effect->getParameter ? effect->getParameter(effect, index) : 0.0f;
        parameterInfos.push_back({std::to_string(index),
                                  parameterName(effect, index), value, 0.0f,
                                  1.0f, value});
        pendingValues[static_cast<size_t>(index)].store(
            value, std::memory_order_relaxed);
      }
      std::vector<bool> assigned(parameterInfos.size(), false);
      for (const auto &overrideValue : overrides) {
        size_t index = parameterInfos.size();
        size_t consumed = 0;
        try {
          const auto numeric = std::stoul(overrideValue.symbol, &consumed);
          if (consumed == overrideValue.symbol.size()) {
            if (numeric >= parameterInfos.size())
              throw std::runtime_error(
                  "VST2: parameter index is out of range: " +
                  overrideValue.symbol);
            index = static_cast<size_t>(numeric);
          }
        } catch (const std::invalid_argument &) {
          consumed = 0;
        } catch (const std::out_of_range &) {
          throw std::runtime_error("VST2: parameter index is out of range: " +
                                   overrideValue.symbol);
        }
        if (index == parameterInfos.size()) {
          for (size_t candidate = 0; candidate < parameterInfos.size();
               ++candidate) {
            if (parameterInfos[candidate].name != overrideValue.symbol)
              continue;
            if (index != parameterInfos.size())
              throw std::runtime_error("VST2: parameter name is ambiguous: " +
                                       overrideValue.symbol);
            index = candidate;
          }
        }
        if (index >= parameterInfos.size() || assigned[index] ||
            !effect->setParameter || !std::isfinite(overrideValue.value) ||
            overrideValue.value < 0.0f || overrideValue.value > 1.0f)
          throw std::runtime_error("VST2: invalid parameter override '" +
                                   overrideValue.symbol + "'");
        assigned[index] = true;
        effect->setParameter(effect, static_cast<int>(index),
                             overrideValue.value);
        parameterInfos[index].value = overrideValue.value;
      }
      if (!source.empty() && (effect->flags & effFlagsProgramChunks)) {
        const fs::path absoluteSource =
            fs::absolute(source).lexically_normal();
        const fs::path absoluteModule =
            fs::absolute(fs::path(modulePath)).lexically_normal();
        persistentIdentity =
            absoluteSource.generic_string() + "\n" +
            std::to_string(sourceLine) + "\n" +
            absoluteModule.generic_string() + "\n" +
            std::to_string(effect->uniqueID) + "\n" +
            std::to_string(effect->version);
        persistentStatePath = statePathFor(persistentIdentity);
        if (auto state = readStateFile(*persistentStatePath,
                                       persistentIdentity)) {
          // Match Equalizer APO's existing VSTPlugin implementation: index 1
          // represents the current program chunk. Its saved state supersedes
          // config-time parameter defaults so live edits survive restarts.
          (void)effect->dispatcher(effect, effSetChunk, 1,
                                   static_cast<t_fstPtrInt>(state->size()),
                                   state->data(), 0.0f);
          for (size_t index = 0; index < parameterInfos.size(); ++index) {
            const float value = effect->getParameter
                                    ? effect->getParameter(
                                          effect, static_cast<int>(index))
                                    : parameterInfos[index].value;
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
              throw std::runtime_error(
                  "VST2 plugin returned an invalid parameter after state "
                  "restore: " + modulePath);
            parameterInfos[index].value = value;
            pendingValues[index].store(value, std::memory_order_relaxed);
          }
        }
      }
      effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
      active = true;
      publishInitialLatency();
    } catch (...) {
      shutdownEffect();
      throw;
    }
  }

  ~VST2Instance() override {
    shutdownEffect();
  }

  void shutdownEffect() noexcept {
    if (effect && opened) {
      if (active)
        effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
      effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    }
    effect = nullptr;
    opened = false;
    active = false;
  }

  const std::string &uri() const noexcept override {
    return modulePath;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return modulePath;
  }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfos;
  }
  bool savePersistentPluginState() override {
    if (persistentIdentity.empty() || !persistentStatePath || !effect ||
        !(effect->flags & effFlagsProgramChunks))
      return false;
    if (failed.load(std::memory_order_acquire))
      throw std::runtime_error("cannot persist failed VST2 plugin state: " +
                               modulePath);

    // The Engine calls this only after audio callback quiescence. Flush the
    // latest block-boundary parameter mailbox before asking the plugin to
    // serialize its current program chunk.
    applyPendingParameters();
    const bool resume = active;
    if (resume) {
      effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
      active = false;
    }
    bool resumed = false;
    try {
      void *pluginChunk = nullptr;
      const t_fstPtrInt reportedSize =
          effect->dispatcher(effect, effGetChunk, 1, 0, &pluginChunk, 0.0f);
      if (reportedSize < 0)
        throw std::runtime_error(
            "VST2 plugin returned a negative program chunk size: " +
            modulePath);
      if (reportedSize == 0) {
        if (resume) {
          effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
          active = true;
          resumed = true;
        }
        return false;
      }
      if (static_cast<uint64_t>(reportedSize) > MaxVST2StateBytes ||
          !pluginChunk)
        throw std::runtime_error(
            "VST2 plugin returned an invalid or oversized program chunk: " +
            modulePath);
      const auto *begin = static_cast<const uint8_t *>(pluginChunk);
      std::vector<uint8_t> state(begin, begin + reportedSize);
      if (resume) {
        effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
        active = true;
        resumed = true;
      }
      atomicWriteState(*persistentStatePath, persistentIdentity, state);
      return true;
    } catch (...) {
      if (resume && !resumed) {
        try {
          effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
          active = true;
        } catch (...) {
          failed.store(true, std::memory_order_release);
        }
      }
      throw;
    }
  }
  void setParameterValue(const std::string &symbol, float value) override {
    if (!effect || !effect->setParameter || !std::isfinite(value))
      throw std::runtime_error("VST2: live parameter control is unavailable");

    size_t parameterIndex = parameterCount;
    size_t consumed = 0;
    try {
      const auto numericIndex = std::stoul(symbol, &consumed);
      if (consumed == symbol.size())
        parameterIndex = static_cast<size_t>(numericIndex);
    } catch (const std::exception &) {
      // Fall back to a unique display-name match.
    }
    if (parameterIndex == parameterCount) {
      for (size_t index = 0; index < parameterInfos.size(); ++index) {
        if (parameterInfos[index].name != symbol)
          continue;
        if (parameterIndex != parameterCount)
          throw std::runtime_error("VST2: parameter name is ambiguous: " +
                                   symbol);
        parameterIndex = index;
      }
    }
    if (parameterIndex >= parameterCount ||
        value < parameterInfos[parameterIndex].minimum ||
        value > parameterInfos[parameterIndex].maximum)
      throw std::runtime_error("VST2: invalid parameter value for '" + symbol +
                               "'");

    pendingValues[parameterIndex].store(value, std::memory_order_relaxed);
    pendingMask[parameterIndex / 64].fetch_or(
        uint64_t{1} << (parameterIndex % 64), std::memory_order_release);
  }
  uint32_t latencySamples() const noexcept override {
    return publishedLatency.load(std::memory_order_acquire);
  }
  bool latencyRefreshPending() const noexcept override {
    return latencyChanged.load(std::memory_order_acquire);
  }
  bool refreshPluginLatency() override {
    if (!latencyChanged.exchange(false, std::memory_order_acq_rel))
      return false;
    const int32_t inputs = effect ? effect->numInputs : -1;
    const int32_t outputs = effect ? effect->numOutputs : -1;
    const int32_t delay = effect ? effect->initialDelay : -1;
    if (inputs != static_cast<int32_t>(channelCount) ||
        outputs != static_cast<int32_t>(channelCount) || delay < 0 ||
        static_cast<uint32_t>(delay) >
            skyapo::plugin::MaxRealtimeLatencySamples) {
      failed.store(true, std::memory_order_release);
      publishedLatency.store(0, std::memory_order_release);
      return true;
    }
    publishedLatency.store(static_cast<uint32_t>(delay),
                           std::memory_order_release);
    return true;
  }
  std::vector<std::wstring>
  initialize(float rate, unsigned maxFrames,
             const std::vector<std::wstring> &channels) override {
    if (rate != context.sampleRate || maxFrames != maxFrameCount ||
        channels.size() != channelCount)
      throw std::runtime_error(
          "VST2: initialize arguments differ from create()");
    return channels;
  }

  void process(float **output, float **input,
               unsigned frames) noexcept override {
    if (!effect || failed.load(std::memory_order_acquire) || !output ||
        !input || frames > maxFrameCount ||
        frames > static_cast<unsigned>(INT32_MAX)) {
      silence(output, frames);
      if (!output || !input || frames > maxFrameCount ||
          frames > static_cast<unsigned>(INT32_MAX))
        failed.store(true, std::memory_order_release);
      return;
    }
    for (size_t channel = 0; channel < channelCount; ++channel) {
      if (!input[channel] || !output[channel]) {
        failed.store(true, std::memory_order_release);
        silence(output, frames);
        return;
      }
    }
    try {
      applyPendingParameters();
      effect->processReplacing(effect, input, output, static_cast<int>(frames));
    } catch (...) {
      failed.store(true, std::memory_order_release);
      silence(output, frames);
    }
  }
  bool processingFailed() const noexcept override {
    return failed.load(std::memory_order_acquire);
  }

private:
  void publishInitialLatency() {
    const int32_t delay = effect ? effect->initialDelay : -1;
    if (delay < 0 || static_cast<uint32_t>(delay) >
                         skyapo::plugin::MaxRealtimeLatencySamples)
      throw std::runtime_error("VST2: invalid or excessive plugin latency: " +
                               modulePath);
    publishedLatency.store(static_cast<uint32_t>(delay),
                           std::memory_order_release);
  }

  void applyPendingParameters() {
    const size_t maskWords = (parameterCount + 63) / 64;
    for (size_t word = 0; word < maskWords; ++word) {
      uint64_t pending =
          pendingMask[word].exchange(0, std::memory_order_acquire);
      while (pending) {
        const unsigned bit = static_cast<unsigned>(__builtin_ctzll(pending));
        const size_t index = word * 64 + bit;
        effect->setParameter(
            effect, static_cast<int>(index),
            pendingValues[index].load(std::memory_order_relaxed));
        pending &= pending - 1;
      }
    }
  }

  void silence(float **output, unsigned frames) noexcept {
    if (!output)
      return;
    // `frames` can be invalidly larger than the caller's negotiated buffer.
    // In that case only the first maxFrameCount samples are known to exist.
    const unsigned safeFrames = std::min(frames, maxFrameCount);
    for (size_t channel = 0; channel < channelCount; ++channel)
      if (output[channel])
        std::fill_n(output[channel], safeFrames, 0.0f);
  }

  std::string modulePath;
  HostContext context;
  size_t channelCount{};
  unsigned maxFrameCount{};
  size_t parameterCount{};
  std::unique_ptr<DynamicModule> module;
  AEffect *effect{};
  std::string persistentIdentity;
  std::optional<fs::path> persistentStatePath;
  bool opened = false;
  bool active = false;
  std::atomic<bool> failed{false};
  std::atomic<bool> latencyChanged{false};
  std::atomic<uint32_t> publishedLatency{0};
  std::vector<PluginParameterInfo> parameterInfos;
  // Values are allocated at construction. The serialized control thread
  // publishes latest-value mailboxes; the audio thread applies changes at a
  // block boundary without allocating or waiting on a lock.
  std::unique_ptr<std::atomic<float>[]> pendingValues;
  std::array<std::atomic<uint64_t>, 64> pendingMask{};
};

} // namespace

std::unique_ptr<IPluginInstance>
VST2PluginHost::create(const std::string &modulePath, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  return std::make_unique<VST2Instance>(modulePath, sampleRate, maxFrames,
                                        channels, parameters, fs::path{}, 0);
}

std::unique_ptr<IPluginInstance> VST2PluginHost::createForConfig(
    const std::string &modulePath, float sampleRate, unsigned maxFrames,
    const std::vector<std::wstring> &channels,
    const std::vector<PluginParameterValue> &parameters,
    const std::filesystem::path &source, unsigned sourceLine) {
  return std::make_unique<VST2Instance>(modulePath, sampleRate, maxFrames,
                                        channels, parameters, source,
                                        sourceLine);
}
