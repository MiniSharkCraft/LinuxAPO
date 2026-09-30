#include "VST2PluginHost.h"

#include "fst.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free,
              "VST2 prototype failure latch must be lock-free on the audio thread");

struct HostContext {
  float sampleRate{};
  unsigned blockSize{};
};

thread_local HostContext *constructingContext = nullptr;

t_fstPtrInt audioMaster(AEffect *effect, int opcode, int, t_fstPtrInt,
                        void *, float) {
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
  default:
    return 0;
  }
}

using PluginMain = AEffect *(*)(audioMasterCallback);

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

class VST2Instance final : public IPluginInstance {
public:
  VST2Instance(std::string path, float rate, unsigned maxFrames,
               const std::vector<std::wstring> &channels,
               const std::vector<PluginParameterValue> &overrides)
      : modulePath(std::move(path)), context{rate, maxFrames},
        channelCount(channels.size()), maxFrameCount(maxFrames) {
    if (!std::isfinite(rate) || rate < 8000.0f || rate > 384000.0f ||
        maxFrames == 0 || (channelCount != 1 && channelCount != 2))
      throw std::runtime_error("VST2 prototype supports mono/stereo and valid "
                               "sample rate/block size only");

    module = std::make_unique<DynamicModule>(modulePath);
    struct ContextGuard {
      explicit ContextGuard(HostContext *value) : previous(constructingContext) {
        constructingContext = value;
      }
      ~ContextGuard() { constructingContext = previous; }
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
      parameterInfos.reserve(static_cast<size_t>(effect->numParams));
      for (int index = 0; index < effect->numParams; ++index) {
        const float value = effect->getParameter
            ? effect->getParameter(effect, index)
            : 0.0f;
        parameterInfos.push_back({std::to_string(index),
                                  parameterName(effect, index), value,
                                  0.0f, 1.0f, value});
      }
      std::vector<bool> assigned(parameterInfos.size(), false);
      for (const auto &overrideValue : overrides) {
        size_t consumed = 0;
        unsigned long index = 0;
        try {
          index = std::stoul(overrideValue.symbol, &consumed);
        } catch (const std::exception &) {
          throw std::runtime_error(
              "VST2: parameter IDs must be decimal indices");
        }
        if (consumed != overrideValue.symbol.size() ||
            index >= parameterInfos.size() || assigned[index] ||
            !effect->setParameter || !std::isfinite(overrideValue.value) ||
            overrideValue.value < 0.0f || overrideValue.value > 1.0f)
          throw std::runtime_error("VST2: invalid parameter override '" +
                                   overrideValue.symbol + "'");
        assigned[index] = true;
        effect->setParameter(effect, static_cast<int>(index),
                             overrideValue.value);
        parameterInfos[index].value = overrideValue.value;
      }
      effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
      active = true;
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

  const std::string &uri() const noexcept override { return modulePath; }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfos;
  }
  std::vector<std::wstring> initialize(float rate, unsigned maxFrames,
      const std::vector<std::wstring> &channels) override {
    if (rate != context.sampleRate || maxFrames != maxFrameCount ||
        channels.size() != channelCount)
      throw std::runtime_error("VST2: initialize arguments differ from create()");
    return channels;
  }

  void process(float **output, float **input, unsigned frames) noexcept override {
    if (!effect || failed.load(std::memory_order_acquire) ||
        !output || !input || frames > maxFrameCount ||
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
  std::unique_ptr<DynamicModule> module;
  AEffect *effect{};
  bool opened = false;
  bool active = false;
  std::atomic<bool> failed{false};
  std::vector<PluginParameterInfo> parameterInfos;
};
} // namespace

std::unique_ptr<IPluginInstance>
VST2PluginHost::create(const std::string &modulePath, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) const {
  return std::make_unique<VST2Instance>(modulePath, sampleRate, maxFrames,
                                        channels, parameters);
}
