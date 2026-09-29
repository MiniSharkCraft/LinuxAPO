#include <lv2/core/lv2.h>
#include <stdlib.h>

typedef struct {
  const float *input[2];
  float *output[2];
  const float *gain;
} TestGain;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle, const LV2_Feature *const *features) {
  (void)descriptor;
  (void)rate;
  (void)bundle;
  (void)features;
  return calloc(1, sizeof(TestGain));
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  TestGain *gain = (TestGain *)instance;
  if (port < 2)
    gain->input[port] = (const float *)data;
  else if (port < 4)
    gain->output[port - 2] = (float *)data;
  else if (port == 4)
    gain->gain = (const float *)data;
}

static void run(LV2_Handle instance, uint32_t frames) {
  TestGain *gain = (TestGain *)instance;
  const float multiplier = gain->gain ? *gain->gain : 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    gain->output[0][i] = gain->input[0][i] * multiplier;
    gain->output[1][i] = gain->input[1][i] * multiplier;
  }
}

static void cleanup(LV2_Handle instance) { free(instance); }

static const LV2_Descriptor descriptor = {
    "https://skyapo.example/plugins/test-gain", instantiate, connect_port,
    NULL, run, NULL, cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
