#include <clap/clap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gain_data {
  const clap_host_t *host;
  double gain;
  uint32_t latency_samples;
  uint32_t latency_cursor;
  float latency_history[2][64];
  bool failFirstProcess;
  unsigned processCalls;
} gain_data;

static bool CLAP_ABI plugin_init(const clap_plugin_t *plugin) {
  if (!plugin || !plugin->plugin_data)
    return false;
  gain_data *data = plugin->plugin_data;
  const clap_host_thread_check_t *thread_check =
      data->host->get_extension(data->host, CLAP_EXT_THREAD_CHECK);
  return thread_check && thread_check->is_main_thread(data->host);
}
static void CLAP_ABI plugin_destroy(const clap_plugin_t *plugin) {
  free(plugin->plugin_data);
  free((void *)plugin);
}
static bool CLAP_ABI plugin_activate(const clap_plugin_t *plugin, double rate,
                                     uint32_t min_frames, uint32_t max_frames) {
  (void)plugin;
  return rate >= 8000.0 && min_frames >= 1 && max_frames >= min_frames;
}
static void CLAP_ABI plugin_deactivate(const clap_plugin_t *plugin) {
  (void)plugin;
}
static bool CLAP_ABI plugin_start(const clap_plugin_t *plugin) {
  (void)plugin;
  return true;
}
static void CLAP_ABI plugin_stop(const clap_plugin_t *plugin) { (void)plugin; }
static void CLAP_ABI plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }
static clap_process_status CLAP_ABI plugin_process(const clap_plugin_t *plugin,
                                                  const clap_process_t *process) {
  gain_data *data = plugin->plugin_data;
  if (data->failFirstProcess && data->processCalls++ == 0)
    return CLAP_PROCESS_ERROR;
  const clap_host_thread_check_t *thread_check =
      data->host->get_extension(data->host, CLAP_EXT_THREAD_CHECK);
  if (!process || process->audio_inputs_count != 1 ||
      process->audio_outputs_count != 1 || !thread_check ||
      !thread_check->is_audio_thread(data->host))
    return CLAP_PROCESS_ERROR;
  for (uint32_t e = 0; process->in_events &&
                        e < process->in_events->size(process->in_events);
       ++e) {
    const clap_event_header_t *header =
        process->in_events->get(process->in_events, e);
    if (header && header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        header->type == CLAP_EVENT_PARAM_VALUE) {
      const clap_event_param_value_t *event =
          (const clap_event_param_value_t *)header;
      if (event->param_id == 7)
        data->gain = event->value;
    }
  }
  if (!data->latency_samples) {
    for (uint32_t c = 0; c < 2; ++c)
      for (uint32_t i = 0; i < process->frames_count; ++i)
        process->audio_outputs[0].data32[c][i] =
            (float)(process->audio_inputs[0].data32[c][i] * data->gain);
  } else {
    for (uint32_t i = 0; i < process->frames_count; ++i) {
      for (uint32_t c = 0; c < 2; ++c) {
        process->audio_outputs[0].data32[c][i] =
            data->latency_history[c][data->latency_cursor] * data->gain;
        data->latency_history[c][data->latency_cursor] =
            process->audio_inputs[0].data32[c][i];
      }
      data->latency_cursor = (data->latency_cursor + 1) % data->latency_samples;
    }
  }
  return CLAP_PROCESS_CONTINUE;
}
static const void *CLAP_ABI plugin_extension(const clap_plugin_t *plugin,
                                             const char *id);
static void CLAP_ABI plugin_main_thread(const clap_plugin_t *plugin) {
  (void)plugin;
}

static uint32_t CLAP_ABI port_count(const clap_plugin_t *plugin, bool input) {
  (void)plugin;
  (void)input;
  return 1;
}
static bool CLAP_ABI port_get(const clap_plugin_t *plugin, uint32_t index,
                              bool input, clap_audio_port_info_t *info) {
  (void)plugin;
  if (index || !info)
    return false;
  memset(info, 0, sizeof(*info));
  info->id = input ? 0 : 1;
  strcpy(info->name, input ? "Input" : "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t audio_ports = {port_count, port_get};
static uint32_t CLAP_ABI param_count(const clap_plugin_t *plugin) {
  (void)plugin;
  return 1;
}
static bool CLAP_ABI param_info(const clap_plugin_t *plugin, uint32_t index,
                                clap_param_info_t *info) {
  (void)plugin;
  if (index || !info)
    return false;
  memset(info, 0, sizeof(*info));
  info->id = 7;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  strcpy(info->name, "Gain");
  info->min_value = 0.0;
  info->max_value = 2.0;
  info->default_value = 0.5;
  return true;
}
static bool CLAP_ABI param_value(const clap_plugin_t *plugin, clap_id id,
                                  double *value) {
  if (id != 7 || !value)
    return false;
  *value = ((const gain_data *)plugin->plugin_data)->gain;
  return true;
}
static bool CLAP_ABI param_value_to_text(const clap_plugin_t *plugin, clap_id id,
                                         double value, char *buffer,
                                         uint32_t capacity) {
  (void)plugin;
  if (id != 7 || !buffer || capacity == 0)
    return false;
  snprintf(buffer, capacity, "%.3f", value);
  return true;
}
static bool CLAP_ABI param_text_to_value(const clap_plugin_t *plugin, clap_id id,
                                         const char *text, double *value) {
  (void)plugin;
  if (id != 7 || !text || !value)
    return false;
  char *end = NULL;
  *value = strtod(text, &end);
  return end != text && *end == '\0';
}
static void CLAP_ABI param_flush(const clap_plugin_t *plugin,
                                const clap_input_events_t *input,
                                const clap_output_events_t *output) {
  (void)plugin;
  (void)input;
  (void)output;
}
static const clap_plugin_params_t plugin_params = {
    param_count,         param_info,          param_value,
    param_value_to_text, param_text_to_value, param_flush};
static uint32_t CLAP_ABI plugin_latency_get(const clap_plugin_t *plugin) {
  return ((const gain_data *)plugin->plugin_data)->latency_samples;
}
static const clap_plugin_latency_t plugin_latency = {plugin_latency_get};
static const void *CLAP_ABI plugin_extension(const clap_plugin_t *plugin,
                                             const char *id) {
  (void)plugin;
  if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
    return &audio_ports;
  if (strcmp(id, CLAP_EXT_PARAMS) == 0)
    return &plugin_params;
  if (strcmp(id, CLAP_EXT_LATENCY) == 0)
    return &plugin_latency;
  return NULL;
}

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "org.skyapo.test.gain",
    .name = "SkyAPO CLAP Test Gain",
    .vendor = "SkyAPO tests",
    .url = "https://example.invalid/skyapo-test-gain",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Test-only stereo half-gain effect",
    .features = NULL};

static const clap_plugin_descriptor_t error_descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "org.skyapo.test.error-once",
    .name = "SkyAPO CLAP Test Error Once",
    .vendor = "SkyAPO tests",
    .url = "https://example.invalid/skyapo-test-error-once",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Test-only CLAP effect that errors on its first process call",
    .features = NULL};

static const clap_plugin_descriptor_t latency_descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "org.skyapo.test.latency",
    .name = "SkyAPO CLAP Test 64-Sample Delay",
    .vendor = "SkyAPO tests",
    .url = "https://example.invalid/skyapo-test-latency",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Test-only CLAP stereo effect with 64 samples of latency",
    .features = NULL};

static const clap_plugin_t *CLAP_ABI create_plugin(
    const clap_plugin_factory_t *factory, const clap_host_t *host,
    const char *plugin_id) {
  (void)factory;
  const bool failFirstProcess =
      strcmp(plugin_id, error_descriptor.id) == 0;
  const bool reportsLatency = strcmp(plugin_id, latency_descriptor.id) == 0;
  if (!failFirstProcess && !reportsLatency &&
      strcmp(plugin_id, descriptor.id) != 0)
    return NULL;
  clap_plugin_t *plugin = calloc(1, sizeof(*plugin));
  gain_data *data = calloc(1, sizeof(*data));
  if (!plugin || !data) {
    free(plugin);
    free(data);
    return NULL;
  }
  data->host = host;
  data->gain = 0.5;
  data->latency_samples = reportsLatency ? 64 : 0;
  data->failFirstProcess = failFirstProcess;
  plugin->desc = failFirstProcess
                     ? &error_descriptor
                     : (reportsLatency ? &latency_descriptor : &descriptor);
  plugin->plugin_data = data;
  plugin->init = plugin_init;
  plugin->destroy = plugin_destroy;
  plugin->activate = plugin_activate;
  plugin->deactivate = plugin_deactivate;
  plugin->start_processing = plugin_start;
  plugin->stop_processing = plugin_stop;
  plugin->reset = plugin_reset;
  plugin->process = plugin_process;
  plugin->get_extension = plugin_extension;
  plugin->on_main_thread = plugin_main_thread;
  return plugin;
}
static uint32_t CLAP_ABI plugin_count(const clap_plugin_factory_t *factory) {
  (void)factory;
  return 3;
}
static const clap_plugin_descriptor_t *CLAP_ABI plugin_descriptor(
    const clap_plugin_factory_t *factory, uint32_t index) {
  (void)factory;
  return index == 0   ? &descriptor
         : index == 1 ? &error_descriptor
         : index == 2 ? &latency_descriptor
                      : NULL;
}
static const clap_plugin_factory_t factory = {plugin_count, plugin_descriptor,
                                               create_plugin};
static bool CLAP_ABI entry_init(const char *path) {
  return path != NULL;
}
static void CLAP_ABI entry_deinit(void) {}
static const void *CLAP_ABI entry_get_factory(const char *id) {
  return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory};
