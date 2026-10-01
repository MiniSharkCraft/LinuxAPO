/* Test-only VST2-compatible gain fixture using only FST's GPL-3-or-later API.
 * Not intended as a distributable audio plugin. */
#include "fst.h"

#include <stdlib.h>
#include <string.h>

#ifdef SKYAPO_VST2_FIXTURE_DYNAMIC_LATENCY
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static const int fixture_io_changed_opcode = audioMasterIOChanged;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

typedef struct {
  float gain;
  float chunk_gain;
  audioMasterCallback host;
  int latency_notified;
  unsigned delay_cursor;
  float delay_line[2][128];
} FixtureState;

static int reported_sample_rate;
static int reported_block_size;
#ifdef SKYAPO_VST2_FIXTURE_MONO
#define FIXTURE_CHANNELS 1
#else
#define FIXTURE_CHANNELS 2
#endif

static t_fstPtrInt dispatch(AEffect *effect, int opcode, int index,
                            t_fstPtrInt value, void *pointer, float opt) {
  (void)opt;
  FixtureState *state = (FixtureState *)effect->resvd1;
  switch (opcode) {
  case effOpen:
  case effSetSampleRate:
  case effSetBlockSize:
  case effMainsChanged:
    return 1;
  case effClose:
    free(state);
    free(effect);
    return 1;
  case effGetParamName:
    if (pointer) {
      strncpy((char *)pointer, "Gain", kVstMaxParamStrLen);
      ((char *)pointer)[kVstMaxParamStrLen] = '\0';
    }
    return 1;
  case effGetChunk:
#ifndef SKYAPO_VST2_FIXTURE_NO_CHUNKS
    if (index == 1 && pointer) {
      state->chunk_gain = state->gain;
      *(void **)pointer = &state->chunk_gain;
      return (t_fstPtrInt)sizeof(state->chunk_gain);
    }
#endif
    return 0;
  case effSetChunk:
#ifndef SKYAPO_VST2_FIXTURE_NO_CHUNKS
    if (index == 1 && value == (t_fstPtrInt)sizeof(float) && pointer) {
      float restored_gain;
      memcpy(&restored_gain, pointer, sizeof(restored_gain));
      if (restored_gain >= 0.0f && restored_gain <= 1.0f) {
        state->gain = restored_gain;
        return 0;
      }
    }
#endif
    return 0;
  default:
    return 0;
  }
}

static void set_parameter(AEffect *effect, int index, float value) {
  FixtureState *state = (FixtureState *)effect->resvd1;
  if (index == 0)
    state->gain = value;
}

static float get_parameter(AEffect *effect, int index) {
  FixtureState *state = (FixtureState *)effect->resvd1;
  return index == 0 ? state->gain : 0.0f;
}

static void process_replacing(AEffect *effect, float **input, float **output,
                              int frames) {
  int channel;
  int frame;
  FixtureState *state = (FixtureState *)effect->resvd1;
#ifdef SKYAPO_VST2_FIXTURE_DYNAMIC_LATENCY
  for (frame = 0; frame < frames; ++frame) {
    const unsigned delay = (unsigned)effect->initialDelay;
    const unsigned read_cursor =
        (state->delay_cursor + 128u - delay) % 128u;
    for (channel = 0; channel < effect->numOutputs; ++channel) {
      const float delayed = state->delay_line[channel][read_cursor];
      state->delay_line[channel][state->delay_cursor] = input[channel][frame];
      output[channel][frame] = delayed * state->gain;
    }
    state->delay_cursor = (state->delay_cursor + 1u) % 128u;
  }
  if (!state->latency_notified) {
    state->latency_notified = 1;
    effect->initialDelay = 64;
    if (state->host)
      (void)state->host(effect, fixture_io_changed_opcode, 0, 0, 0, 0.0f);
  }
#else
  for (channel = 0; channel < effect->numOutputs; ++channel)
    for (frame = 0; frame < frames; ++frame)
      output[channel][frame] = input[channel][frame] * state->gain;
#endif
}

AEffect *VSTPluginMain(audioMasterCallback callback) {
  AEffect *fixture = calloc(1, sizeof(*fixture));
  FixtureState *state = calloc(1, sizeof(*state));
  if (!fixture || !state) {
    free(fixture);
    free(state);
    return NULL;
  }
  state->gain = 0.5f;
  state->host = callback;
  fixture->resvd1 = (t_fstPtrInt)state;
  fixture->magic = kEffectMagic;
  fixture->dispatcher = dispatch;
  fixture->setParameter = set_parameter;
  fixture->getParameter = get_parameter;
  fixture->numParams = 1;
  fixture->numInputs = FIXTURE_CHANNELS;
  fixture->numOutputs = FIXTURE_CHANNELS;
  fixture->flags = effFlagsCanReplacing;
#ifndef SKYAPO_VST2_FIXTURE_NO_CHUNKS
  fixture->flags |= effFlagsProgramChunks;
#endif
  fixture->uniqueID = 0x534b4150;
#ifdef SKYAPO_VST2_FIXTURE_DYNAMIC_LATENCY
  fixture->initialDelay = 32;
#endif
  fixture->version = 1;
  fixture->processReplacing = process_replacing;
  if (callback) {
    (void)callback(fixture, audioMasterVersion, 0, 0, 0, 0.0f);
    /* During the entry call fixture.user is still NULL by design. */
    reported_sample_rate = (int)callback(
        fixture, audioMasterGetSampleRate, 0, 0, 0, 0.0f);
    reported_block_size = (int)callback(
        fixture, audioMasterGetBlockSize, 0, 0, 0, 0.0f);
  }
  return fixture;
}

int vst2FixtureReportedSampleRate(void) { return reported_sample_rate; }
int vst2FixtureReportedBlockSize(void) { return reported_block_size; }
