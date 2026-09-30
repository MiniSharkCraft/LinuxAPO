// SkyAPO's small VST3 integration fixture. The implementation is original
// test code; it uses Steinberg's SDK interfaces but copies no SDK sample code.
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <array>
#include <cmath>
#include <cstdint>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
class TestGain final : public SingleComponentEffect {
public:
  explicit TestGain(bool failFirstProcess = false)
      : failFirstProcess(failFirstProcess) {}

  static FUnknown *create(void *) {
    return static_cast<IAudioProcessor *>(new TestGain);
  }
  static FUnknown *createErrorOnce(void *) {
    return static_cast<IAudioProcessor *>(new TestGain(true));
  }

  tresult PLUGIN_API initialize(FUnknown *context) override {
    const auto result = SingleComponentEffect::initialize(context);
    if (result != kResultOk)
      return result;
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    parameters.addParameter(USTRING("Gain"), nullptr, kStepCountContinuous, 0.5,
                            ParameterInfo::kCanAutomate, 7);
    return kResultOk;
  }

  tresult PLUGIN_API setProcessing(TBool) override {
    return kResultOk;
  }

  tresult PLUGIN_API getState(IBStream *state) override {
    if (!state)
      return kInvalidArgument;
    uint32_t magic = 0x534B5956;
    float savedGain = gain;
    int32 writtenMagic = 0;
    int32 writtenGain = 0;
    if (state->write(&magic, sizeof(magic), &writtenMagic) != kResultOk ||
        state->write(&savedGain, sizeof(savedGain), &writtenGain) !=
            kResultOk ||
        writtenMagic != sizeof(magic) || writtenGain != sizeof(savedGain))
      return kResultFalse;
    return kResultOk;
  }

  tresult PLUGIN_API setState(IBStream *state) override {
    if (!state)
      return kInvalidArgument;
    uint32_t magic = 0;
    float restoredGain = 0.0f;
    int32 readMagic = 0;
    int32 readGain = 0;
    if (state->read(&magic, sizeof(magic), &readMagic) != kResultOk ||
        state->read(&restoredGain, sizeof(restoredGain), &readGain) !=
            kResultOk ||
        readMagic != sizeof(magic) || readGain != sizeof(restoredGain) ||
        magic != 0x534B5956 || !std::isfinite(restoredGain) ||
        restoredGain < 0.0f || restoredGain > 1.0f)
      return kResultFalse;
    gain = restoredGain;
    return kResultOk;
  }

  tresult PLUGIN_API process(ProcessData &data) override {
    if (failFirstProcess && !failedOnce) {
      failedOnce = true;
      return kResultFalse;
    }
    if (data.numInputs != 1 || data.numOutputs != 1 || !data.inputs ||
        !data.outputs || data.inputs[0].numChannels != 2 ||
        data.outputs[0].numChannels != 2 ||
        data.symbolicSampleSize != kSample32)
      return kResultFalse;
    if (data.inputParameterChanges) {
      for (int32 index = 0;
           index < data.inputParameterChanges->getParameterCount(); ++index) {
        auto *queue = data.inputParameterChanges->getParameterData(index);
        if (!queue || queue->getParameterId() != 7 ||
            queue->getPointCount() == 0)
          continue;
        int32 sampleOffset = 0;
        ParamValue value = gain;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) ==
            kResultTrue)
          gain = static_cast<float>(value);
      }
    }
    for (int32 channel = 0; channel < 2; ++channel) {
      const auto *input = data.inputs[0].channelBuffers32[channel];
      auto *output = data.outputs[0].channelBuffers32[channel];
      for (int32 frame = 0; frame < data.numSamples; ++frame)
        output[frame] = input[frame] * gain;
    }
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
  }

private:
  float gain = 0.5f;
  bool failFirstProcess = false;
  bool failedOnce = false;
};

class TestLatencyDelay final : public SingleComponentEffect {
public:
  static constexpr uint32_t DelaySamples = 64;

  static FUnknown *create(void *) {
    return static_cast<IAudioProcessor *>(new TestLatencyDelay);
  }

  tresult PLUGIN_API initialize(FUnknown *context) override {
    const auto result = SingleComponentEffect::initialize(context);
    if (result != kResultOk)
      return result;
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    return kResultOk;
  }

  tresult PLUGIN_API setProcessing(TBool) override {
    return kResultOk;
  }
  uint32 PLUGIN_API getLatencySamples() override {
    return DelaySamples;
  }

  tresult PLUGIN_API process(ProcessData &data) override {
    if (data.numInputs != 1 || data.numOutputs != 1 || !data.inputs ||
        !data.outputs || data.inputs[0].numChannels != 2 ||
        data.outputs[0].numChannels != 2 ||
        data.symbolicSampleSize != kSample32)
      return kResultFalse;
    for (int32 frame = 0; frame < data.numSamples; ++frame) {
      for (int32 channel = 0; channel < 2; ++channel) {
        auto *input = data.inputs[0].channelBuffers32[channel];
        auto *output = data.outputs[0].channelBuffers32[channel];
        output[frame] = history[channel][cursor];
        history[channel][cursor] = input[frame];
      }
      cursor = (cursor + 1) % DelaySamples;
    }
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
  }

private:
  std::array<std::array<float, DelaySamples>, 2> history{};
  uint32_t cursor{};
};
} // namespace

BEGIN_FACTORY("SkyAPO Tests", "https://example.invalid/skyapo", "", 0)
DEF_CLASS2(INLINE_UID(0x534B5941, 0x504F0001, 0x00000000, 0x00000001),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SkyAPO Test Half Gain", Vst::kDistributable, "Fx", "1.0.0",
           kVstVersionString, TestGain::create)
DEF_CLASS2(INLINE_UID(0x534B5941, 0x504F0001, 0x00000000, 0x00000002),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SkyAPO Test Error Once", Vst::kDistributable, "Fx", "1.0.0",
           kVstVersionString, TestGain::createErrorOnce)
DEF_CLASS2(INLINE_UID(0x534B5941, 0x504F0001, 0x00000000, 0x00000003),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SkyAPO Test 64-Sample Delay", Vst::kDistributable, "Fx", "1.0.0",
           kVstVersionString, TestLatencyDelay::create)
END_FACTORY
