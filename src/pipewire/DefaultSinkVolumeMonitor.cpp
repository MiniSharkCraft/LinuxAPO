#include "DefaultSinkVolumeMonitor.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/keys.h>
#include <pipewire/permission.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/json.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace skyapo::pipewire {
namespace {
constexpr const char *DefaultMetadataName = "default";
constexpr const char *DefaultSinkKey = "default.audio.sink";

bool sameState(const DefaultSinkVolumeSnapshot &a,
               const DefaultSinkVolumeSnapshot &b) {
  return a.stableName == b.stableName && a.available == b.available &&
         a.muted == b.muted && a.effectiveGain == b.effectiveGain &&
         (a.effectiveDb == b.effectiveDb ||
          (std::isinf(a.effectiveDb) && std::isinf(b.effectiveDb)));
}
} // namespace

struct DefaultSinkVolumeMonitor::Impl {
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  pw_metadata *metadata = nullptr;
  pw_node *node = nullptr;
  Callback callback = nullptr;
  void *userData = nullptr;
  uint32_t metadataId = SPA_ID_INVALID;
  uint32_t nodeId = SPA_ID_INVALID;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> sinks;
  std::string wantedName;
  DefaultSinkVolumeSnapshot current;
  spa_hook registryHook{};
  spa_hook metadataHook{};
  spa_hook nodeHook{};

  void publish(DefaultSinkVolumeSnapshot next) {
    if (sameState(current, next))
      return;
    next.generation = current.generation + 1;
    current = std::move(next);
    if (callback)
      callback(userData, current);
  }

  void forgetNode() {
    if (node) {
      spa_hook_remove(&nodeHook);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node));
      node = nullptr;
    }
    nodeId = SPA_ID_INVALID;
    auto next = current;
    next.available = false;
    next.muted = false;
    next.effectiveGain = 0.0f;
    next.effectiveDb = -std::numeric_limits<float>::infinity();
    publish(std::move(next));
  }

  void select(const std::string &name) {
    if (name == wantedName && (node || name.empty()))
      return;
    forgetNode();
    wantedName = name;
    auto next = current;
    next.stableName = wantedName;
    next.available = false;
    next.muted = false;
    next.effectiveGain = 0.0f;
    next.effectiveDb = -std::numeric_limits<float>::infinity();
    publish(std::move(next));
    if (!wantedName.empty())
      findNode();
  }

  void findNode() {
    if (node || wantedName.empty())
      return;
    const auto it = sinks.find(wantedName);
    if (it == sinks.end())
      return;
    const auto [id, version] = it->second;
    auto *object = static_cast<pw_node *>(pw_registry_bind(
        registry, id, PW_TYPE_INTERFACE_Node,
        std::min(version, static_cast<uint32_t>(PW_VERSION_NODE)), 0));
    if (!object)
      return;
    node = object;
    nodeId = id;
    const int listenerResult =
        pw_node_add_listener(object, &nodeHook, &nodeEvents, this);
    if (listenerResult < 0)
      std::cerr << "skyapod: cannot listen to default sink node ("
                << listenerResult << ")\n";
  }

  void queryNodeParams() {
    if (!node)
      return;
    uint32_t params[] = {SPA_PARAM_Props};
    const int subscribeResult = pw_node_subscribe_params(node, params, 1);
    const int enumResult =
        pw_node_enum_params(node, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    if (subscribeResult < 0 || enumResult < 0)
      std::cerr << "skyapod: cannot query default render endpoint Props ("
                << "subscribe=" << subscribeResult
                << ", enum=" << enumResult << ")\n";
  }

  static void onNodeInfo(void *data, const pw_node_info *) {
    auto &self = *static_cast<Impl *>(data);
    self.queryNodeParams();
  }

  static void onRegistryGlobal(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const spa_dict *props) {
    auto &self = *static_cast<Impl *>(data);
    if (!type || !props)
      return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
        self.metadataId == SPA_ID_INVALID &&
        spa_dict_lookup(props, PW_KEY_METADATA_NAME) &&
        std::strcmp(spa_dict_lookup(props, PW_KEY_METADATA_NAME),
                    DefaultMetadataName) == 0) {
      auto *object = static_cast<pw_metadata *>(pw_registry_bind(
          self.registry, id, PW_TYPE_INTERFACE_Metadata,
          std::min(version, static_cast<uint32_t>(PW_VERSION_METADATA)), 0));
      if (!object)
        return;
      self.metadata = object;
      self.metadataId = id;
      pw_metadata_add_listener(object, &self.metadataHook, &metadataEvents,
                               &self);
      return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      const char *media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
      if (!name || !media || std::strcmp(media, "Audio/Sink") != 0)
        return;
      if (!PW_PERM_IS_X(permissions)) {
        std::cerr << "skyapod: cannot query default render endpoint '" << name
                  << "': PipeWire did not grant X permission\n";
        return;
      }
      self.sinks[name] = {id, version};
      self.findNode();
    }
  }

  static void onRegistryRemove(void *data, uint32_t id) {
    auto &self = *static_cast<Impl *>(data);
    auto it = std::find_if(self.sinks.begin(), self.sinks.end(),
                           [id](const auto &item) {
                             return item.second.first == id;
                           });
    if (it != self.sinks.end()) {
      const bool wasSelected = it->first == self.wantedName;
      self.sinks.erase(it);
      if (wasSelected) {
        self.forgetNode();
        self.findNode();
      }
    }
    if (id == self.metadataId) {
      if (self.metadata) {
        spa_hook_remove(&self.metadataHook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(self.metadata));
        self.metadata = nullptr;
      }
      self.metadataId = SPA_ID_INVALID;
      self.select({});
    }
  }

  static int onMetadataProperty(void *data, uint32_t subject, const char *key,
                                const char *, const char *value) {
    auto &self = *static_cast<Impl *>(data);
    if (subject != 0 || !key || std::strcmp(key, DefaultSinkKey) != 0)
      return 0;
    std::string name;
    if (!value || !DefaultSinkVolumeMonitor::parseDefaultSinkMetadata(value, name))
      self.select({});
    else
      self.select(name);
    return 0;
  }

  static void onNodeParam(void *data, int, uint32_t id, uint32_t,
                         uint32_t, const spa_pod *param) {
    auto &self = *static_cast<Impl *>(data);
    if (!self.node || id != SPA_PARAM_Props || !param)
      return;
    // Node enumeration may also yield unrelated device Props (for example
    // ALSA card metadata). Do not let those overwrite a valid volume snapshot.
    if (!spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
      return;
    auto next = self.current;
    if (!DefaultSinkVolumeMonitor::parseProps(param, next)) {
      next.available = false;
      next.muted = false;
      next.effectiveGain = 0.0f;
      next.effectiveDb = -std::numeric_limits<float>::infinity();
    } else {
      next.available = true;
    }
    self.publish(std::move(next));
  }

  static const pw_registry_events registryEvents;
  static const pw_metadata_events metadataEvents;
  static const pw_node_events nodeEvents;
};

const pw_registry_events DefaultSinkVolumeMonitor::Impl::registryEvents = {
    PW_VERSION_REGISTRY_EVENTS, &Impl::onRegistryGlobal,
    &Impl::onRegistryRemove};
const pw_metadata_events DefaultSinkVolumeMonitor::Impl::metadataEvents = {
    PW_VERSION_METADATA_EVENTS, &Impl::onMetadataProperty};
const pw_node_events DefaultSinkVolumeMonitor::Impl::nodeEvents = {
    PW_VERSION_NODE_EVENTS, &Impl::onNodeInfo, &Impl::onNodeParam};

DefaultSinkVolumeMonitor::DefaultSinkVolumeMonitor(pw_core *core,
                                                   pw_registry *registry,
                                                   Callback callback,
                                                   void *userData)
    : impl_(new Impl) {
  impl_->core = core;
  impl_->registry = registry;
  impl_->callback = callback;
  impl_->userData = userData;
  impl_->current.effectiveDb = -std::numeric_limits<float>::infinity();
  pw_registry_add_listener(registry, &impl_->registryHook,
                           &Impl::registryEvents, impl_);
}

DefaultSinkVolumeMonitor::~DefaultSinkVolumeMonitor() {
  if (impl_->node) {
    spa_hook_remove(&impl_->nodeHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl_->node));
  }
  if (impl_->metadata) {
    spa_hook_remove(&impl_->metadataHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl_->metadata));
  }
  spa_hook_remove(&impl_->registryHook);
  delete impl_;
}

const DefaultSinkVolumeSnapshot &DefaultSinkVolumeMonitor::snapshot() const noexcept {
  return impl_->current;
}

bool DefaultSinkVolumeMonitor::parseDefaultSinkMetadata(
    const char *value, std::string &stableName) {
  if (!value)
    return false;
  char name[1024]{};
  if (spa_json_str_object_find(value, std::strlen(value), "name", name,
                               sizeof(name)) <= 0 || name[0] == '\0')
    return false;
  stableName.assign(name);
  return true;
}

bool DefaultSinkVolumeMonitor::parseProps(
    const spa_pod *pod, DefaultSinkVolumeSnapshot &snapshot) noexcept {
  if (!pod || !spa_pod_is_object(pod))
    return false;
  const auto *muteProp = spa_pod_find_prop(pod, nullptr, SPA_PROP_mute);
  const auto *volProp = spa_pod_find_prop(pod, nullptr, SPA_PROP_channelVolumes);
  if (!volProp)
    return false;
  bool muted = false;
  if (muteProp && spa_pod_get_bool(&muteProp->value, &muted) < 0)
    return false;
  uint32_t count = 0, size = 0, type = 0;
  const auto *values = static_cast<const uint8_t *>(spa_pod_get_array_full(
      &volProp->value, &count, &size, &type));
  if (!values || count == 0 || type != SPA_TYPE_Float ||
      size != sizeof(float))
    return false;
  double power = 0.0;
  for (uint32_t i = 0; i < count; ++i) {
    float volume;
    std::memcpy(&volume, values + static_cast<size_t>(i) * size,
                sizeof(volume));
    if (!std::isfinite(volume) || volume < 0.0f)
      return false;
    power += static_cast<double>(volume) * volume;
  }
  const float gain = static_cast<float>(std::sqrt(power / count));
  snapshot.effectiveGain = gain;
  snapshot.muted = muted || gain == 0.0f;
  snapshot.effectiveDb = gain == 0.0f
                             ? -std::numeric_limits<float>::infinity()
                             : 20.0f * std::log10(gain);
  return true;
}

} // namespace skyapo::pipewire
