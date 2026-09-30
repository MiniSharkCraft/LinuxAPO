#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

typedef struct gain_data { const clap_host_t *host; } gain_data;

static bool CLAP_ABI plugin_init(const clap_plugin_t *plugin) {
  if (!plugin || !plugin->plugin_data)
    return false;
  const gain_data *data = plugin->plugin_data;
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
  const gain_data *data = plugin->plugin_data;
  const clap_host_thread_check_t *thread_check =
      data->host->get_extension(data->host, CLAP_EXT_THREAD_CHECK);
  if (!process || process->audio_inputs_count != 1 ||
      process->audio_outputs_count != 1 || !thread_check ||
      !thread_check->is_audio_thread(data->host))
    return CLAP_PROCESS_ERROR;
  for (uint32_t c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < process->frames_count; ++i)
      process->audio_outputs[0].data32[c][i] =
          process->audio_inputs[0].data32[c][i] * 0.5f;
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
static const void *CLAP_ABI plugin_extension(const clap_plugin_t *plugin,
                                             const char *id) {
  (void)plugin;
  return strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0 ? &audio_ports : NULL;
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

static const clap_plugin_t *CLAP_ABI create_plugin(
    const clap_plugin_factory_t *factory, const clap_host_t *host,
    const char *plugin_id) {
  (void)factory;
  if (strcmp(plugin_id, descriptor.id) != 0)
    return NULL;
  clap_plugin_t *plugin = calloc(1, sizeof(*plugin));
  gain_data *data = calloc(1, sizeof(*data));
  if (!plugin || !data) {
    free(plugin);
    free(data);
    return NULL;
  }
  data->host = host;
  plugin->desc = &descriptor;
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
  return 1;
}
static const clap_plugin_descriptor_t *CLAP_ABI plugin_descriptor(
    const clap_plugin_factory_t *factory, uint32_t index) {
  (void)factory;
  return index == 0 ? &descriptor : NULL;
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
