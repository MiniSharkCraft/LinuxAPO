#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const float *input[2];
  float *output[2];
  unsigned channels;
  float *latency;
  float ring[2][64];
  unsigned cursor;
  unsigned delay;
  int invalid;
  int oversize;
} TestLatency;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle,
                              const LV2_Feature *const *features) {
  (void)rate;
  (void)bundle;
  (void)features;
  TestLatency *state = calloc(1, sizeof(TestLatency));
  if (state) {
    state->delay = 64;
    state->channels =
        !strcmp(descriptor->URI,
                "https://skyapo.example/plugins/test-latency-mono")
            ? 1
            : 2;
    state->invalid =
        !strcmp(descriptor->URI,
                "https://skyapo.example/plugins/test-latency-invalid");
    state->oversize =
        !strcmp(descriptor->URI,
                "https://skyapo.example/plugins/test-latency-oversize");
  }
  return state;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  TestLatency *state = (TestLatency *)instance;
  if (port < state->channels)
    state->input[port] = (const float *)data;
  else if (port < 2 * state->channels)
    state->output[port - state->channels] = (float *)data;
  else if (port == 2 * state->channels)
    state->latency = (float *)data;
}

static void run(LV2_Handle instance, uint32_t frames) {
  TestLatency *state = (TestLatency *)instance;
  for (uint32_t i = 0; i < frames; ++i) {
    for (unsigned channel = 0; channel < state->channels; ++channel) {
      state->output[channel][i] = state->ring[channel][state->cursor];
      state->ring[channel][state->cursor] = state->input[channel][i];
    }
    state->cursor = (state->cursor + 1) % (state->delay ? state->delay : 64);
  }
  if (frames && !state->invalid && state->delay == 64) {
    state->delay = 32;
    state->cursor = 0;
    memset(state->ring, 0, sizeof(state->ring));
  }
  if (state->latency)
    *state->latency = state->invalid && frames
                          ? NAN
                          : (state->oversize && frames ? 10001.0f
                                                       : (float)state->delay);
}

static void cleanup(LV2_Handle instance) { free(instance); }

static const LV2_Descriptor legacy = {
    "https://skyapo.example/plugins/test-latency-legacy", instantiate,
    connect_port, NULL, run, NULL, cleanup, NULL};
static const LV2_Descriptor designated = {
    "https://skyapo.example/plugins/test-latency-designated", instantiate,
    connect_port, NULL, run, NULL, cleanup, NULL};
static const LV2_Descriptor invalid = {
    "https://skyapo.example/plugins/test-latency-invalid", instantiate,
    connect_port, NULL, run, NULL, cleanup, NULL};
static const LV2_Descriptor mono = {
    "https://skyapo.example/plugins/test-latency-mono", instantiate,
    connect_port, NULL, run, NULL, cleanup, NULL};
static const LV2_Descriptor oversize = {
    "https://skyapo.example/plugins/test-latency-oversize", instantiate,
    connect_port, NULL, run, NULL, cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  return index == 0 ? &legacy
         : index == 1 ? &designated
         : index == 2 ? &invalid
         : index == 3 ? &mono
         : index == 4 ? &oversize
                      : NULL;
}
