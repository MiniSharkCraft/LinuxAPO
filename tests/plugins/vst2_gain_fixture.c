/* Test-only VST2-compatible gain fixture using only FST's GPL-3-or-later API.
 * Not intended as a distributable audio plugin. */
#include "fst.h"

#include <string.h>

static AEffect fixture;
static float gain = 0.5f;
static int reported_sample_rate;
static int reported_block_size;
#ifdef SKYAPO_VST2_FIXTURE_MONO
#define FIXTURE_CHANNELS 1
#else
#define FIXTURE_CHANNELS 2
#endif

static t_fstPtrInt dispatch(AEffect *effect, int opcode, int index,
                            t_fstPtrInt value, void *pointer, float opt) {
  (void)effect;
  (void)index;
  (void)value;
  (void)opt;
  switch (opcode) {
  case effOpen:
  case effClose:
  case effSetSampleRate:
  case effSetBlockSize:
  case effMainsChanged:
    return 1;
  case effGetParamName:
    if (pointer) {
      strncpy((char *)pointer, "Gain", kVstMaxParamStrLen);
      ((char *)pointer)[kVstMaxParamStrLen] = '\0';
    }
    return 1;
  default:
    return 0;
  }
}

static void set_parameter(AEffect *effect, int index, float value) {
  (void)effect;
  if (index == 0)
    gain = value;
}

static float get_parameter(AEffect *effect, int index) {
  (void)effect;
  return index == 0 ? gain : 0.0f;
}

static void process_replacing(AEffect *effect, float **input, float **output,
                              int frames) {
  int channel;
  int frame;
  (void)effect;
  for (channel = 0; channel < effect->numOutputs; ++channel)
    for (frame = 0; frame < frames; ++frame)
      output[channel][frame] = input[channel][frame] * gain;
}

AEffect *VSTPluginMain(audioMasterCallback callback) {
  memset(&fixture, 0, sizeof(fixture));
  fixture.magic = kEffectMagic;
  fixture.dispatcher = dispatch;
  fixture.setParameter = set_parameter;
  fixture.getParameter = get_parameter;
  fixture.numParams = 1;
  fixture.numInputs = FIXTURE_CHANNELS;
  fixture.numOutputs = FIXTURE_CHANNELS;
  fixture.flags = effFlagsCanReplacing;
  fixture.processReplacing = process_replacing;
  if (callback) {
    (void)callback(&fixture, audioMasterVersion, 0, 0, 0, 0.0f);
    /* During the entry call fixture.user is still NULL by design. */
    reported_sample_rate = (int)callback(
        &fixture, audioMasterGetSampleRate, 0, 0, 0, 0.0f);
    reported_block_size = (int)callback(
        &fixture, audioMasterGetBlockSize, 0, 0, 0, 0.0f);
  }
  return &fixture;
}

int vst2FixtureReportedSampleRate(void) { return reported_sample_rate; }
int vst2FixtureReportedBlockSize(void) { return reported_block_size; }
