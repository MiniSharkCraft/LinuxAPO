#include "LV2PluginHost.h"

#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include <cmath>
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
LilvWorld *processWorld() {
  // Keep one catalog for the process, not one catalog per Engine/reload. The
  // installed Lilv 0.28 build leaves a 24-byte allocation from
  // lilv_world_load_plugin_classes unreachable when a loaded world is freed.
  // A process-lifetime world avoids repeated leaked metadata and also avoids
  // rescanning every bundle during each graph rebuild; the OS reclaims it at
  // process exit.
  static LilvWorld *world = [] {
    LilvWorld *created = lilv_world_new();
    if (created)
      lilv_world_load_all(created);
    return created;
  }();
  return world;
}

class LV2Instance final : public IPluginInstance {
  enum class PortKind { AudioInput, AudioOutput, Control };
  struct Port {
    PortKind kind{};
    uint32_t index{};
    unsigned channel{};
    float control{};
  };

public:
  LV2Instance(LilvWorld *world, const LilvPlugin *plugin, std::string uri,
              float sampleRate, unsigned maxFrames,
              const std::vector<std::wstring> &channels)
      : pluginUri(std::move(uri)), maxFrameCount(maxFrames) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0f || !maxFrames ||
        channels.empty())
      throw std::runtime_error("invalid LV2 instance audio configuration");

    LilvNode *audioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode *controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode *inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode *outputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
    LilvNode *latencyProperty = lilv_new_uri(world, LV2_CORE__reportsLatency);
    if (!audioPort || !controlPort || !inputPort || !outputPort ||
        !latencyProperty) {
      lilv_node_free(audioPort);
      lilv_node_free(controlPort);
      lilv_node_free(inputPort);
      lilv_node_free(outputPort);
      lilv_node_free(latencyProperty);
      throw std::runtime_error("cannot create LV2 port class URIs");
    }

    LilvNodes *required = lilv_plugin_get_required_features(plugin);
    if (required && lilv_nodes_size(required)) {
      const LilvNode *feature = lilv_nodes_get_first(required);
      const std::string featureUri =
          feature ? lilv_node_as_uri(feature) : "unknown feature";
      lilv_nodes_free(required);
      cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                   latencyProperty);
      throw std::runtime_error("LV2 plugin requires unsupported host feature " +
                               featureUri);
    }
    lilv_nodes_free(required);

    ports.resize(lilv_plugin_get_num_ports(plugin));
    unsigned audioInputs = 0, audioOutputs = 0;
    for (uint32_t i = 0; i < ports.size(); ++i) {
      const LilvPort *descriptor = lilv_plugin_get_port_by_index(plugin, i);
      const bool audio = lilv_port_is_a(plugin, descriptor, audioPort);
      const bool control = lilv_port_is_a(plugin, descriptor, controlPort);
      const bool input = lilv_port_is_a(plugin, descriptor, inputPort);
      const bool output = lilv_port_is_a(plugin, descriptor, outputPort);
      if (lilv_port_has_property(plugin, descriptor, latencyProperty)) {
        cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                     latencyProperty);
        throw std::runtime_error(
            "LV2 plugin reports latency, but plugin delay compensation is not "
            "implemented: " +
            pluginUri);
      }
      if (audio && input) {
        ports[i].kind = PortKind::AudioInput;
        ports[i].index = i;
        ports[i].channel = audioInputs++;
      } else if (audio && output) {
        ports[i].kind = PortKind::AudioOutput;
        ports[i].index = i;
        ports[i].channel = audioOutputs++;
      } else if (control) {
        if (!input && !output) {
          cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                       latencyProperty);
          throw std::runtime_error("LV2 control port has no direction");
        }
        ports[i].kind = PortKind::Control;
        ports[i].index = i;
        LilvNode *def = nullptr, *minimum = nullptr, *maximum = nullptr;
        lilv_port_get_range(plugin, descriptor, &def, &minimum, &maximum);
        if (def && lilv_node_is_float(def))
          ports[i].control = lilv_node_as_float(def);
        else if (def && lilv_node_is_int(def))
          ports[i].control = static_cast<float>(lilv_node_as_int(def));
        lilv_node_free(def);
        lilv_node_free(minimum);
        lilv_node_free(maximum);
      } else {
        cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                     latencyProperty);
        const LilvNode *name = lilv_port_get_name(plugin, descriptor);
        throw std::runtime_error(std::string("LV2 port '") +
                                 (name ? lilv_node_as_string(name) : "?") +
                                 "' is not a supported audio/control port");
      }
    }
    cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                 latencyProperty);
    if (audioInputs != channels.size() || audioOutputs != channels.size())
      throw std::runtime_error("LV2 plugin audio port layout is " +
                               std::to_string(audioInputs) + " in / " +
                               std::to_string(audioOutputs) + " out; SkyAPO " +
                               std::to_string(channels.size()) +
                               "-channel processing requires matching counts");

    instance = lilv_plugin_instantiate(plugin, sampleRate, nullptr);
    if (!instance)
      throw std::runtime_error("LV2 plugin instantiate failed: " + pluginUri);
    for (auto &port : ports)
      if (port.kind == PortKind::Control)
        lilv_instance_connect_port(instance, port.index, &port.control);
    lilv_instance_activate(instance);
    active = true;
  }

  ~LV2Instance() override {
    if (instance) {
      if (active)
        lilv_instance_deactivate(instance);
      lilv_instance_free(instance);
    }
  }

  const std::string &uri() const noexcept override { return pluginUri; }

  std::vector<std::wstring>
  initialize(float, unsigned,
             const std::vector<std::wstring> &channels) override {
    return channels;
  }

  void process(float **output, float **input,
               unsigned frames) noexcept override {
    if (!instance || frames > maxFrameCount)
      return;
    for (const auto &port : ports) {
      if (port.kind == PortKind::AudioInput)
        lilv_instance_connect_port(instance, port.index, input[port.channel]);
      else if (port.kind == PortKind::AudioOutput)
        lilv_instance_connect_port(instance, port.index, output[port.channel]);
    }
    lilv_instance_run(instance, frames);
  }

private:
  static void cleanupNodes(LilvNode *a, LilvNode *b, LilvNode *c, LilvNode *d,
                           LilvNode *e) {
    lilv_node_free(a);
    lilv_node_free(b);
    lilv_node_free(c);
    lilv_node_free(d);
    lilv_node_free(e);
  }

  std::string pluginUri;
  unsigned maxFrameCount;
  std::vector<Port> ports;
  LilvInstance *instance{};
  bool active = false;
};

class LV2PluginFilter final : public IFilter {
public:
  LV2PluginFilter(LV2PluginHost &host, std::string uri)
      : host(host), pluginUri(std::move(uri)) {}

  bool getInPlace() override { return false; }

  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrameCount,
             std::vector<std::wstring> channelNames) override {
    instance = host.create(pluginUri, sampleRate, maxFrameCount, channelNames);
    return instance->initialize(sampleRate, maxFrameCount, channelNames);
  }

  void process(float **output, float **input, unsigned frames) override {
    instance->process(output, input, frames);
  }

private:
  LV2PluginHost &host;
  std::string pluginUri;
  std::unique_ptr<IPluginInstance> instance;
};

IFilter *allocatePluginFilter(LV2PluginHost &host, std::string uri) {
  void *memory = MemoryHelper::alloc(sizeof(LV2PluginFilter));
  try {
    return new (memory) LV2PluginFilter(host, std::move(uri));
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}
} // namespace

LV2PluginHost::LV2PluginHost() = default;

LV2PluginHost::~LV2PluginHost() = default;

std::unique_ptr<IPluginInstance>
LV2PluginHost::create(const std::string &uri, float sampleRate,
                      unsigned maxFrames,
                      const std::vector<std::wstring> &channels) {
  LilvWorld *world = processWorld();
  if (!world)
    throw std::runtime_error("cannot create Lilv world");
  LilvNode *node = lilv_new_uri(world, uri.c_str());
  if (!node)
    throw std::runtime_error("invalid LV2 plugin URI: " + uri);
  const LilvPlugin *plugin =
      lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), node);
  lilv_node_free(node);
  if (!plugin)
    throw std::runtime_error("LV2 plugin not found: " + uri);
  return std::make_unique<LV2Instance>(world, plugin, uri, sampleRate,
                                       maxFrames, channels);
}

std::vector<std::pair<std::string, std::string>> LV2PluginHost::list() const {
  LilvWorld *world = processWorld();
  if (!world)
    throw std::runtime_error("cannot create Lilv world");
  std::vector<std::pair<std::string, std::string>> result;
  const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
  for (LilvIter *i = lilv_plugins_begin(plugins);
       !lilv_plugins_is_end(plugins, i); i = lilv_plugins_next(plugins, i)) {
    const LilvPlugin *plugin = lilv_plugins_get(plugins, i);
    const LilvNode *uri = lilv_plugin_get_uri(plugin);
    LilvNode *name = lilv_plugin_get_name(plugin);
    result.emplace_back(uri ? lilv_node_as_uri(uri) : "",
                        name ? lilv_node_as_string(name) : "(unnamed)");
    lilv_node_free(name);
  }
  return result;
}

class LV2PluginFilterFactory final : public IFilterFactory {
public:
  LV2PluginFilterFactory() : host(std::make_unique<LV2PluginHost>()) {}

  std::vector<IFilter *> createFilter(const std::wstring &,
                                      std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, wideUri, extra;
    input >> format >> wideUri;
    if (format != L"LV2")
      throw std::runtime_error("Plugin: currently supports only LV2 <URI>");
    if (wideUri.empty() || input >> extra)
      throw std::runtime_error("expected Plugin: LV2 <plugin-URI>");
    const auto uri = StringHelper::toString(wideUri, 65001);
    return {allocatePluginFilter(*host, uri)};
  }

private:
  std::unique_ptr<LV2PluginHost> host;
};

std::unique_ptr<IFilterFactory> makeLV2PluginFilterFactory() {
  return std::make_unique<LV2PluginFilterFactory>();
}
