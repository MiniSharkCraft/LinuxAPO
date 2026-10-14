#include "DeviceManager.h"
#include "DefaultSinkVolumeMonitor.h"
#include "NodeIdParser.h"
#include "helpers/StringHelper.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <pipewire/extensions/metadata.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <stdexcept>
#include <utility>
namespace {
constexpr const char *DefaultMetadataName = "default";
constexpr const char *DefaultSourceKey = "default.audio.source";

std::string prop(const spa_dict *p, const char *k) {
  auto *v = spa_dict_lookup(p, k);
  return v ? v : "";
}
struct State {
  Devices devices;
  struct NodeBinding {
    uint32_t id{};
    pw_node *node{};
    spa_hook hook{};
    State *state{};
  };
  pw_registry *registry{};
  pw_metadata *metadata{};
  uint32_t metadataId{SPA_ID_INVALID};
  std::string defaultSourceName;
  bool defaultSourceMetadataSeen = false;
  std::vector<std::unique_ptr<NodeBinding>> nodeBindings;
  spa_hook metadataHook{};
  int done = -1;
  bool error = false;
};
int defaultSourceMetadataProperty(void *metadataData, uint32_t subject,
                                  const char *key, const char *,
                                  const char *value) {
  auto &state = *static_cast<State *>(metadataData);
  if (subject != 0 || !key || std::strcmp(key, DefaultSourceKey) != 0)
    return 0;
  state.defaultSourceMetadataSeen = true;
  std::string name;
  if (skyapo::pipewire::DefaultSinkVolumeMonitor::parseDefaultNodeMetadata(
          value, name))
    state.defaultSourceName = std::move(name);
  else
    state.defaultSourceName.clear();
  return 0;
}
const pw_metadata_events metadataEvents = {PW_VERSION_METADATA_EVENTS,
                                           &defaultSourceMetadataProperty};

void nodeInfo(void *data, const pw_node_info *info) {
  if (!info || !info->props)
    return;
  auto &binding = *static_cast<State::NodeBinding *>(data);
  auto &sources = binding.state->devices.sources;
  const auto device = std::find_if(
      sources.begin(), sources.end(),
      [&](const AudioDevice &candidate) { return candidate.id == binding.id; });
  if (device == sources.end())
    return;
  const auto identity = skyapo::pipewire::deviceIdentityFromProps(info->props);
  if (!identity.serial.empty())
    device->identity.serial = identity.serial;
  if (!identity.busId.empty())
    device->identity.busId = identity.busId;
  if (!identity.alsaCard.empty())
    device->identity.alsaCard = identity.alsaCard;
  if (!identity.alsaPath.empty())
    device->identity.alsaPath = identity.alsaPath;
}
const pw_node_events nodeEvents = [] {
  pw_node_events value{};
  value.version = PW_VERSION_NODE_EVENTS;
  value.info = nodeInfo;
  return value;
}();
void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t,
            const spa_dict *props) {
  if (!props)
    return;
  auto &s = *static_cast<State *>(data);
  if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
      prop(props, PW_KEY_METADATA_NAME) == DefaultMetadataName && !s.metadata) {
    s.metadata = static_cast<pw_metadata *>(pw_registry_bind(
        s.registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (s.metadata) {
      s.metadataId = id;
      pw_metadata_add_listener(s.metadata, &s.metadataHook, &metadataEvents,
                               &s);
    }
    return;
  }
  if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
      prop(props, PW_KEY_MEDIA_CLASS) == "Audio/Source") {
    AudioDevice device{id, prop(props, PW_KEY_NODE_NAME),
                       prop(props, PW_KEY_NODE_DESCRIPTION), 0, 0, {}};
    device.identity = skyapo::pipewire::deviceIdentityFromProps(props);
    const auto channels = prop(props, PW_KEY_AUDIO_CHANNELS);
    const auto sampleRate = prop(props, PW_KEY_AUDIO_RATE);
    try {
      if (!channels.empty())
        device.channels = std::stoul(channels);
      if (!sampleRate.empty())
        device.sampleRate = std::stoul(sampleRate);
    } catch (const std::exception &) {
      // Some session managers publish nonnumeric metadata; port enumeration
      // below supplies a reliable channel count fallback.
    }
    s.devices.sources.push_back(std::move(device));
    auto binding = std::make_unique<State::NodeBinding>();
    binding->id = id;
    binding->state = &s;
    binding->node = static_cast<pw_node *>(pw_registry_bind(
        s.registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (binding->node) {
      if (pw_node_add_listener(binding->node, &binding->hook, &nodeEvents,
                               binding.get()) >= 0) {
        s.nodeBindings.push_back(std::move(binding));
      } else {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(binding->node));
      }
    }
  }
  if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
    const auto nodeId = skyapo::pipewire::parseNodeId(
        prop(props, PW_KEY_NODE_ID));
    if (nodeId)
      s.devices.ports.push_back({id, *nodeId,
                                 prop(props, PW_KEY_PORT_DIRECTION),
                                 prop(props, PW_KEY_AUDIO_CHANNEL),
                                 prop(props, PW_KEY_PORT_NAME)});
  }
}
void done(void *d, uint32_t id, int seq) {
  if (id == PW_ID_CORE)
    static_cast<State *>(d)->done = seq;
}
void removed(void *data, uint32_t id) {
  auto &state = *static_cast<State *>(data);
  if (id == state.metadataId) {
    if (state.metadata) {
      spa_hook_remove(&state.metadataHook);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(state.metadata));
      state.metadata = nullptr;
    }
    state.metadataId = SPA_ID_INVALID;
    state.defaultSourceName.clear();
    state.defaultSourceMetadataSeen = false;
  }
}
void error(void *d, uint32_t, int, int, const char *) {
  static_cast<State *>(d)->error = true;
}
} // namespace
Devices enumerateDevices() {
  pw_init(nullptr, nullptr);
  State s;
  auto *loop = pw_main_loop_new(nullptr);
  auto *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
  auto *core = pw_context_connect(context, nullptr, 0);
  if (!core) {
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();
    throw std::runtime_error("cannot connect to PipeWire");
  }
  pw_core_events ce{};
  ce.version = PW_VERSION_CORE_EVENTS;
  ce.done = done;
  ce.error = error;
  pw_registry_events re{};
  re.version = PW_VERSION_REGISTRY_EVENTS;
  re.global = global;
  re.global_remove = removed;
  spa_hook ch{}, rh{};
  pw_core_add_listener(core, &ch, &ce, &s);
  auto *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  s.registry = registry;
  pw_registry_add_listener(registry, &rh, &re, &s);
  int seq = pw_core_sync(core, PW_ID_CORE, 0);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (s.done != seq && !s.error &&
         std::chrono::steady_clock::now() < deadline)
    pw_loop_iterate(pw_main_loop_get_loop(loop), 100);
  if (s.done == seq && !s.error) {
    // Node-specific properties (including stable hardware identity) may be
    // published in pw_node_info rather than the registry global dictionary.
    // A second sync orders replies to the node binds after their info events.
    s.done = -1;
    seq = pw_core_sync(core, PW_ID_CORE, 0);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (s.done != seq && !s.error &&
           std::chrono::steady_clock::now() < deadline)
      pw_loop_iterate(pw_main_loop_get_loop(loop), 100);
  }
  bool ok = s.done == seq && !s.error;
  for (auto &binding : s.nodeBindings) {
    spa_hook_remove(&binding->hook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(binding->node));
  }
  if (s.metadata) {
    spa_hook_remove(&s.metadataHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(s.metadata));
  }
  spa_hook_remove(&rh);
  spa_hook_remove(&ch);
  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_main_loop_destroy(loop);
  pw_deinit();
  if (!ok)
    throw std::runtime_error("PipeWire enumeration failed or timed out");
  for (auto &device : s.devices.sources)
    if (!device.channels)
      device.channels = std::count_if(
          s.devices.ports.begin(), s.devices.ports.end(),
          [&](const AudioPort &port) {
            return port.node == device.id && port.direction == "out";
          });
  markDefaultCaptureSource(
      s.devices, s.defaultSourceMetadataSeen && !s.defaultSourceName.empty()
                     ? std::optional<std::string>(s.defaultSourceName)
                     : std::nullopt);
  return s.devices;
}

std::wstring deviceMatchText(const AudioDevice &device) {
  std::string text = "node.name=" + device.name;
  if (!device.description.empty())
    text += " device.description=" + device.description;
  if (!device.identity.serial.empty())
    text += " device.serial=" + device.identity.serial;
  if (!device.identity.busId.empty())
    text += " device.bus-id=" + device.identity.busId;
  if (!device.identity.alsaCard.empty())
    text += " api.alsa.card=" + device.identity.alsaCard;
  if (!device.identity.alsaPath.empty())
    text += " api.alsa.path=" + device.identity.alsaPath;
  return StringHelper::toWString(text, 65001);
}
