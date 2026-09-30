// SkyAPO's small VST3 integration fixture. The implementation is original
// test code; it uses Steinberg's SDK interfaces but copies no SDK sample code.
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

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
    return kResultOk;
  }

  tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }

  tresult PLUGIN_API process(ProcessData &data) override {
    if (data.numInputs != 1 || data.numOutputs != 1 || !data.inputs ||
        !data.outputs || data.inputs[0].numChannels != 2 ||
        data.outputs[0].numChannels != 2 || data.symbolicSampleSize != kSample32)
      return kResultFalse;
    for (int32 channel = 0; channel < 2; ++channel) {
      const auto *input = data.inputs[0].channelBuffers32[channel];
      auto *output = data.outputs[0].channelBuffers32[channel];
      for (int32 frame = 0; frame < data.numSamples; ++frame)
        output[frame] = input[frame] * 0.5f;
    }
    data.outputs[0].silenceFlags = 0;
    return kResultOk;
  }
};
} // namespace

BEGIN_FACTORY("SkyAPO Tests", "https://example.invalid/skyapo", "", 0)
DEF_CLASS2(INLINE_UID(0x534B5941, 0x504F0001, 0x00000000, 0x00000001),
           PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SkyAPO Test Half Gain", Vst::kDistributable, "Fx",
           "1.0.0", kVstVersionString, TestGain::create)
END_FACTORY
