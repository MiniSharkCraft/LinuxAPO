// SkyAPO's small VST3 integration fixture. The implementation is original
// test code; it uses Steinberg's SDK interfaces but copies no SDK sample code.
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
class TestGain final : public SingleComponentEffect {
public:
  static FUnknown *create(void *) { return static_cast<IAudioProcessor *>(new TestGain); }

  tresult PLUGIN_API initialize(FUnknown *context) override {
    const auto result = SingleComponentEffect::initialize(context);
    if (result != kResultOk)
      return result;
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    parameters.addParameter(USTRING("Gain"), nullptr, kStepCountContinuous,
                            0.5, ParameterInfo::kCanAutomate, 7);
    return kResultOk;
  }

  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }

  tresult PLUGIN_API process(ProcessData &data) override {
    if (data.numInputs != 1 || data.numOutputs != 1 || !data.inputs ||
        !data.outputs || data.inputs[0].numChannels != 2 ||
        data.outputs[0].numChannels != 2 || data.symbolicSampleSize != kSample32)
      return kResultFalse;
    if (data.inputParameterChanges) {
      for (int32 index = 0; index < data.inputParameterChanges->getParameterCount(); ++index) {
        auto *queue = data.inputParameterChanges->getParameterData(index);
        if (!queue || queue->getParameterId() != 7 || queue->getPointCount() == 0)
          continue;
        int32 sampleOffset = 0;
        ParamValue value = gain;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) == kResultTrue)
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
};
} // namespace

BEGIN_FACTORY("SkyAPO Tests", "https://example.invalid/skyapo", "", 0)
DEF_CLASS2(INLINE_UID(0x534B5941, 0x504F0001, 0x00000000, 0x00000001),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SkyAPO Test Half Gain", Vst::kDistributable, "Fx",
           "1.0.0", kVstVersionString, TestGain::create)
END_FACTORY
