#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const float *input[2];
  float *output[2];
  float *gain;
  float *enabled;
  LV2_URID_Map *map;
} TestGain;

static int last_enabled = -1;
static unsigned process_calls = 0;

LV2_SYMBOL_EXPORT int skyapo_test_lv2_last_enabled(void) {
  return last_enabled;
}

LV2_SYMBOL_EXPORT unsigned skyapo_test_lv2_process_calls(void) {
  return process_calls;
}

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle, const LV2_Feature *const *features) {
  (void)descriptor;
  (void)rate;
  (void)bundle;
  TestGain *state = (TestGain *)calloc(1, sizeof(TestGain));
  if (state && features) {
    for (unsigned i = 0; features[i]; ++i)
      if (!strcmp(features[i]->URI, LV2_URID__map))
        state->map = (LV2_URID_Map *)features[i]->data;
  }
  return state;
}

static LV2_State_Status save_state(LV2_Handle instance,
                                   LV2_State_Store_Function store,
                                   LV2_State_Handle handle, uint32_t flags,
                                   const LV2_Feature *const *features) {
  (void)flags; (void)features;
  TestGain *gain = (TestGain *)instance;
  if (!gain->map || !gain->gain) return LV2_STATE_ERR_NO_FEATURE;
  const LV2_URID key = gain->map->map(gain->map->handle,
      "https://skyapo.example/plugins/test-gain#saved-gain");
  const LV2_URID type = gain->map->map(gain->map->handle, LV2_ATOM__Float);
  return store(handle, key, gain->gain, sizeof(float), type,
               LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status restore_state(LV2_Handle instance,
                                      LV2_State_Retrieve_Function retrieve,
                                      LV2_State_Handle handle, uint32_t flags,
                                      const LV2_Feature *const *features) {
  (void)flags; (void)features;
  TestGain *gain = (TestGain *)instance;
  if (!gain->map || !gain->gain) return LV2_STATE_ERR_NO_FEATURE;
  const LV2_URID key = gain->map->map(gain->map->handle,
      "https://skyapo.example/plugins/test-gain#saved-gain");
  size_t size = 0;
  uint32_t type = 0;
  const void *value = retrieve(handle, key, &size, &type, NULL);
  if (!value || size != sizeof(float) ||
      type != gain->map->map(gain->map->handle, LV2_ATOM__Float))
    return LV2_STATE_ERR_NO_PROPERTY;
  *(float *)gain->gain = *(const float *)value;
  return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface state_interface = {save_state, restore_state};
static const void *extension_data(const char *uri) {
  return strcmp(uri, LV2_STATE__interface) == 0 ? &state_interface : NULL;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  TestGain *gain = (TestGain *)instance;
  if (port < 2)
    gain->input[port] = (const float *)data;
  else if (port < 4)
    gain->output[port - 2] = (float *)data;
  else if (port == 4)
    gain->gain = (float *)data;
  else if (port == 5)
    gain->enabled = (float *)data;
}

static void run(LV2_Handle instance, uint32_t frames) {
  TestGain *gain = (TestGain *)instance;
  const int enabled = gain->enabled ? (int)*gain->enabled : 1;
  last_enabled = enabled;
  ++process_calls;
  const float multiplier = gain->gain ? *gain->gain : 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    gain->output[0][i] = gain->input[0][i] * (enabled ? multiplier : 1.0f);
    gain->output[1][i] = gain->input[1][i] * (enabled ? multiplier : 1.0f);
  }
}

static void cleanup(LV2_Handle instance) { free(instance); }

static const LV2_Descriptor descriptor = {
    "https://skyapo.example/plugins/test-gain", instantiate, connect_port,
    NULL, run, NULL, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
