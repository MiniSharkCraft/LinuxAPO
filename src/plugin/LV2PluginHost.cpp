#include "LV2PluginHost.h"

#include "IFilter.h"
#include "IPluginLatencyRefresh.h"
#include "IPluginFailureState.h"
#include "IPluginLatencyState.h"
#include "IPluginParameterControl.h"
#include "IPluginBypassControl.h"
#include "IPluginSourceContext.h"
#include "IPluginStatePersistence.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>
#include <lv2/atom/atom.h>
#include <limits>
#include <lv2/core/lv2.h>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <array>
#include <memory>
#include <mutex>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
constexpr size_t MaxStateBytes = 16 * 1024 * 1024;
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "LV2 realtime latency snapshots require lock-free atomics");

struct UridMapper {
  LV2_URID_Map map{this, mapUri};
  LV2_URID_Unmap unmap{this, unmapUrid};
  static constexpr size_t Capacity = 65536;
  std::array<std::atomic<const std::string *>, Capacity> uris{};
  std::mutex mutex;
  std::unordered_map<std::string, LV2_URID> byUri;
  std::vector<std::unique_ptr<std::string>> ownedUris;
  static thread_local bool audioThread;
  static LV2_URID mapUri(LV2_URID_Map_Handle handle, const char *uri) {
    if (!handle || !uri || !*uri)
      return 0;
    auto &self = *static_cast<UridMapper *>(handle);
    for (LV2_URID id = 1; id < Capacity; ++id) {
      const auto *known = self.uris[id].load(std::memory_order_acquire);
      if (!known)
        break;
      if (*known == uri)
        return id;
    }
    // LV2 URIDs are expected to be mapped during instantiation. Unknown URIs
    // requested from run() fail closed instead of allocating or blocking.
    if (audioThread)
      return 0;
    std::lock_guard<std::mutex> lock(self.mutex);
    const auto found = self.byUri.find(uri);
    if (found != self.byUri.end())
      return found->second;
    LV2_URID id = static_cast<LV2_URID>(self.ownedUris.size() + 1);
    if (id >= Capacity)
      return 0;
    std::unique_ptr<std::string> entry;
    try {
      entry = std::make_unique<std::string>(uri);
      const auto *stable = entry.get();
      self.byUri.emplace(*stable, id);
      self.ownedUris.emplace_back(std::move(entry));
      self.uris[id].store(stable, std::memory_order_release);
    } catch (...) {
      self.byUri.erase(uri);
      return 0;
    }
    return id;
  }
  static const char *unmapUrid(LV2_URID_Unmap_Handle handle, LV2_URID id) {
    if (!handle)
      return nullptr;
    auto &self = *static_cast<UridMapper *>(handle);
    if (!id || id >= Capacity)
      return nullptr;
    const auto *known = self.uris[id].load(std::memory_order_acquire);
    return known ? known->c_str() : nullptr;
  }
};
thread_local bool UridMapper::audioThread = false;
UridMapper &uridMapper() {
  static UridMapper mapper;
  return mapper;
}

uint64_t stateHash(const std::string &value) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}
std::string stateIdentity(const fs::path &source, unsigned line,
                          const std::string &uri) {
  return source.generic_string() + "\n" + std::to_string(line) + "\n" + uri;
}
fs::path statePath(const std::string &identity) {
  fs::path base;
  if (const char *xdg = std::getenv("XDG_STATE_HOME");
      xdg && *xdg && fs::path(xdg).is_absolute())
    base = xdg;
  else if (const char *home = std::getenv("HOME"); home && *home)
    base = fs::path(home) / ".local" / "state";
  else
    throw std::runtime_error(
        "LV2 state persistence needs absolute XDG_STATE_HOME or HOME");
  std::ostringstream name;
  name << std::hex << std::setw(16) << std::setfill('0') << stateHash(identity)
       << ".ttl";
  return base / "skyapo" / "lv2-state" / name.str();
}
std::string readState(const fs::path &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT)
      return {};
    throw std::runtime_error("cannot open LV2 state: " + path.string() + ": " +
                             std::strerror(errno));
  }
  struct Guard {
    int fd;
    ~Guard() {
      close(fd);
    }
  } guard{fd};
  struct stat st{};
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0077) || st.st_size <= 0 ||
      static_cast<uint64_t>(st.st_size) > MaxStateBytes)
    throw std::runtime_error("invalid or oversized LV2 state file: " +
                             path.string());
  std::string data(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < data.size()) {
    const auto n = ::read(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("short read from LV2 state file");
    offset += static_cast<size_t>(n);
  }
  char extra{};
  ssize_t extraBytes;
  do {
    extraBytes = ::read(fd, &extra, 1);
  } while (extraBytes < 0 && errno == EINTR);
  if (extraBytes != 0)
    throw std::runtime_error("LV2 state file changed while being read: " +
                             path.string());
  return data;
}
void atomicWriteState(const fs::path &path, const std::string &data) {
  if (data.size() > MaxStateBytes)
    throw std::runtime_error("LV2 state exceeds 16 MiB limit");
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec || fs::is_symlink(fs::symlink_status(path.parent_path(), ec)) || ec)
    throw std::runtime_error("cannot safely create LV2 state directory: " +
                             path.parent_path().string());
  if (chmod(path.parent_path().c_str(), 0700) < 0)
    throw std::runtime_error("cannot secure LV2 state directory");
  std::string pattern = path.string() + ".tmp-XXXXXX";
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  const int fd = mkstemp(temp.data());
  if (fd < 0)
    throw std::runtime_error("cannot create LV2 state temporary file");
  bool renamed = false;
  bool fdOpen = true;
  try {
    if (fchmod(fd, 0600) < 0)
      throw std::runtime_error("cannot secure LV2 state file");
    size_t offset = 0;
    while (offset < data.size()) {
      const auto n = ::write(fd, data.data() + offset, data.size() - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("cannot write LV2 state file");
      offset += static_cast<size_t>(n);
    }
    if (fsync(fd) < 0)
      throw std::runtime_error("cannot sync LV2 state file");
    const int closeResult = close(fd);
    fdOpen = false;
    if (closeResult < 0)
      throw std::runtime_error("cannot close LV2 state file");
    if (rename(temp.data(), path.c_str()) < 0)
      throw std::runtime_error("cannot atomically replace LV2 state file");
    renamed = true;
    const int dir =
        open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
      throw std::runtime_error("cannot open LV2 state directory for sync");
    if (fsync(dir) < 0) {
      close(dir);
      throw std::runtime_error("cannot sync LV2 state directory");
    }
    close(dir);
  } catch (...) {
    if (fdOpen)
      close(fd);
    if (!renamed)
      unlink(temp.data());
    throw;
  }
}

static_assert(std::atomic<float>::is_always_lock_free,
              "LV2 live parameter mailboxes must be lock-free");

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

class LV2Instance final : public IPluginInstance,
                          public IPluginParameterControl,
                          public IPluginStatePersistence,
                          public IPluginLatencyState,
                          public IPluginLatencyRefresh,
                          public IPluginFailureState {
  enum class PortKind { AudioInput, AudioOutput, Control, LatencyOutput };
  struct Port {
    PortKind kind{};
    uint32_t index{};
    unsigned channel{};
    float control{};
    bool controlInput{};
    std::string symbol;
  };
  struct LiveParameter {
    std::string symbol;
    std::string name;
    uint32_t portIndex{};
    float minimum{};
    float maximum{};
    std::atomic<float> value{0.0f};
  };

public:
  LV2Instance(LilvWorld *world, const LilvPlugin *plugin, std::string uri,
              float sampleRate, unsigned maxFrames,
              const std::vector<std::wstring> &channels,
              const std::vector<PluginParameterValue> &overrides,
              std::string identity = {})
      : pluginUri(std::move(uri)), worldRef(world), pluginRef(plugin),
        maxFrameCount(maxFrames),
        channelCount(static_cast<unsigned>(channels.size())),
        persistentIdentity(std::move(identity)) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0f || !maxFrames ||
        channels.empty())
      throw std::runtime_error("invalid LV2 instance audio configuration");

    LilvNode *audioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode *controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode *inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode *outputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
    LilvNode *latencyProperty = lilv_new_uri(world, LV2_CORE__reportsLatency);
    LilvNode *designationProperty = lilv_new_uri(world, LV2_CORE__designation);
    LilvNode *latencyDesignation = lilv_new_uri(world, LV2_CORE__latency);
    if (!audioPort || !controlPort || !inputPort || !outputPort ||
        !latencyProperty || !designationProperty || !latencyDesignation) {
      lilv_node_free(audioPort);
      lilv_node_free(controlPort);
      lilv_node_free(inputPort);
      lilv_node_free(outputPort);
      lilv_node_free(latencyProperty);
      lilv_node_free(designationProperty);
      lilv_node_free(latencyDesignation);
      throw std::runtime_error("cannot create LV2 port class URIs");
    }

    LilvNodes *required = lilv_plugin_get_required_features(plugin);
    if (required) {
      for (LilvIter *i = lilv_nodes_begin(required);
           !lilv_nodes_is_end(required, i); i = lilv_nodes_next(required, i)) {
        const LilvNode *feature = lilv_nodes_get(required, i);
        const std::string featureUri =
            feature ? lilv_node_as_uri(feature) : "unknown feature";
        if (featureUri != LV2_URID__map && featureUri != LV2_URID__unmap) {
          lilv_nodes_free(required);
          cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                       latencyProperty, designationProperty,
                       latencyDesignation);
          throw std::runtime_error(
              "LV2 plugin requires unsupported host feature " + featureUri);
        }
      }
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
      LilvNodes *designations =
          lilv_port_get_value(plugin, descriptor, designationProperty);
      bool designatedLatency = false;
      if (designations) {
        for (LilvIter *designation = lilv_nodes_begin(designations);
             !lilv_nodes_is_end(designations, designation);
             designation = lilv_nodes_next(designations, designation))
          designatedLatency |= lilv_node_equals(
              lilv_nodes_get(designations, designation), latencyDesignation);
        lilv_nodes_free(designations);
      }
      const bool reportsLatency =
          lilv_port_has_property(plugin, descriptor, latencyProperty) ||
          designatedLatency;
      if (reportsLatency) {
        if (!control || !output || latencyPortIndex >= 0) {
          cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                       latencyProperty, designationProperty,
                       latencyDesignation);
          throw std::runtime_error(
              "LV2 plugin must have exactly one latency output ControlPort: " +
              pluginUri);
        }
        latencyPortIndex = static_cast<int32_t>(i);
        ports[i].kind = PortKind::LatencyOutput;
        ports[i].index = i;
        ports[i].controlInput = false;
        ports[i].control = 0.0f;
        continue;
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
                       latencyProperty, designationProperty,
                       latencyDesignation);
          throw std::runtime_error("LV2 control port has no direction");
        }
        ports[i].kind = PortKind::Control;
        ports[i].index = i;
        ports[i].controlInput = input;
        if (input) {
          const LilvNode *symbol = lilv_port_get_symbol(plugin, descriptor);
          if (!symbol) {
            cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                         latencyProperty, designationProperty,
                         latencyDesignation);
            throw std::runtime_error("LV2 input control port has no symbol");
          }
          ports[i].symbol = lilv_node_as_string(symbol);
        }
        LilvNode *def = nullptr, *minimum = nullptr, *maximum = nullptr;
        lilv_port_get_range(plugin, descriptor, &def, &minimum, &maximum);
        float minValue = -std::numeric_limits<float>::infinity();
        float maxValue = std::numeric_limits<float>::infinity();
        float defaultValue = 0.0f;
        if (def && lilv_node_is_float(def))
          defaultValue = lilv_node_as_float(def);
        else if (def && lilv_node_is_int(def))
          defaultValue = static_cast<float>(lilv_node_as_int(def));
        if (minimum && lilv_node_is_float(minimum))
          minValue = lilv_node_as_float(minimum);
        else if (minimum && lilv_node_is_int(minimum))
          minValue = static_cast<float>(lilv_node_as_int(minimum));
        if (maximum && lilv_node_is_float(maximum))
          maxValue = lilv_node_as_float(maximum);
        else if (maximum && lilv_node_is_int(maximum))
          maxValue = static_cast<float>(lilv_node_as_int(maximum));
        ports[i].control = defaultValue;
        if (input) {
          LilvNode *name = lilv_port_get_name(plugin, descriptor);
          const std::string displayName =
              name ? lilv_node_as_string(name) : ports[i].symbol;
          parameterInfos.push_back({ports[i].symbol, displayName, defaultValue,
                                    minValue, maxValue, defaultValue});
          auto parameter = std::make_unique<LiveParameter>();
          parameter->symbol = ports[i].symbol;
          parameter->name = displayName;
          parameter->portIndex = i;
          parameter->minimum = minValue;
          parameter->maximum = maxValue;
          parameter->value.store(defaultValue, std::memory_order_relaxed);
          liveParameters.push_back(std::move(parameter));
          lilv_node_free(name);
        }
        lilv_node_free(def);
        lilv_node_free(minimum);
        lilv_node_free(maximum);
      } else {
        cleanupNodes(audioPort, controlPort, inputPort, outputPort,
                     latencyProperty, designationProperty, latencyDesignation);
        const LilvNode *name = lilv_port_get_name(plugin, descriptor);
        throw std::runtime_error(std::string("LV2 port '") +
                                 (name ? lilv_node_as_string(name) : "?") +
                                 "' is not a supported audio/control port");
      }
    }
    cleanupNodes(audioPort, controlPort, inputPort, outputPort, latencyProperty,
                 designationProperty, latencyDesignation);
    if (audioInputs != channels.size() || audioOutputs != channels.size())
      throw std::runtime_error("LV2 plugin audio port layout is " +
                               std::to_string(audioInputs) + " in / " +
                               std::to_string(audioOutputs) + " out; SkyAPO " +
                               std::to_string(channels.size()) +
                               "-channel processing requires matching counts");

    for (const auto &override : overrides) {
      if (!std::isfinite(override.value))
        throw std::runtime_error("LV2 parameter '" + override.symbol +
                                 "' must be finite");
      auto info = std::find_if(
          parameterInfos.begin(), parameterInfos.end(),
          [&](const auto &p) { return p.symbol == override.symbol; });
      if (info == parameterInfos.end())
        throw std::runtime_error("unknown LV2 input parameter '" +
                                 override.symbol + "'");
      if (override.value < info->minimum || override.value > info->maximum)
        throw std::runtime_error("LV2 parameter '" + override.symbol +
                                 "' value is outside its declared range");
      auto port = std::find_if(ports.begin(), ports.end(), [&](const auto &p) {
        return p.kind == PortKind::Control && p.controlInput &&
               p.symbol == override.symbol;
      });
      if (port == ports.end())
        throw std::runtime_error("LV2 parameter metadata mismatch for '" +
                                 override.symbol + "'");
      port->control = override.value;
      info->value = override.value;
      auto live = std::find_if(liveParameters.begin(), liveParameters.end(),
                               [&](const auto &parameter) {
                                 return parameter->symbol == override.symbol;
                               });
      if (live != liveParameters.end())
        (*live)->value.store(override.value, std::memory_order_relaxed);
    }

    auto &mapper = uridMapper();
    mapFeature = {LV2_URID__map, &mapper.map};
    unmapFeature = {LV2_URID__unmap, &mapper.unmap};
    features[0] = &mapFeature;
    features[1] = &unmapFeature;
    features[2] = nullptr;
    instance = lilv_plugin_instantiate(plugin, sampleRate, features);
    if (!instance)
      throw std::runtime_error("LV2 plugin instantiate failed: " + pluginUri);
    auto cleanupInstance = [this](LilvInstance *created) {
      if (active)
        lilv_instance_deactivate(created);
      lilv_instance_free(created);
      instance = nullptr;
      active = false;
    };
    std::unique_ptr<LilvInstance, decltype(cleanupInstance)> instanceGuard(
        instance, cleanupInstance);
    stateInterface = static_cast<const LV2_State_Interface *>(
        lilv_instance_get_extension_data(instance, LV2_STATE__interface));
    for (auto &port : ports)
      if (port.kind == PortKind::Control ||
          port.kind == PortKind::LatencyOutput)
        lilv_instance_connect_port(instance, port.index, &port.control);
    if (!persistentIdentity.empty()) {
      savedStatePath = statePath(persistentIdentity);
      const auto saved = readState(savedStatePath);
      if (!saved.empty()) {
        if (!stateInterface)
          throw std::runtime_error("LV2 state exists but plugin does not "
                                   "implement state:interface: " +
                                   pluginUri);
        LilvState *state =
            lilv_state_new_from_string(world, &mapper.map, saved.c_str());
        if (!state)
          throw std::runtime_error("corrupt LV2 state file: " +
                                   savedStatePath.string());
        const LilvNode *statePlugin = lilv_state_get_plugin_uri(state);
        const char *storedIdentity = lilv_state_get_label(state);
        if (!statePlugin ||
            std::string(lilv_node_as_uri(statePlugin)) != pluginUri ||
            !storedIdentity || storedIdentity != persistentIdentity) {
          lilv_state_free(state);
          throw std::runtime_error("LV2 state identity mismatch: " +
                                   savedStatePath.string());
        }
        lilv_state_restore(state, instance, setStatePort, this, 0, features);
        lilv_state_free(state);
        for (const auto &parameter : liveParameters) {
          const float value = ports[parameter->portIndex].control;
          if (!std::isfinite(value) || value < parameter->minimum ||
              value > parameter->maximum)
            throw std::runtime_error(
                "LV2 state contains an invalid value for '" +
                parameter->symbol + "': " + pluginUri);
        }
      }
    }
    lilv_instance_activate(instance);
    active = true;
    if (latencyPortIndex >= 0) {
      // LV2 defines run(0) as the way to update immediate output controls,
      // including the initial latency value, without processing audio.
      lilv_instance_run(instance, 0);
      uint32_t initialLatency{};
      if (!readLatency(initialLatency))
        throw std::runtime_error("LV2 plugin reports an invalid initial "
                                 "latency value: " +
                                 pluginUri);
      observedLatency.store(initialLatency, std::memory_order_relaxed);
      publishedLatency.store(initialLatency, std::memory_order_relaxed);
    }
    instanceGuard.release();
  }

  ~LV2Instance() override {
    if (instance) {
      if (active)
        lilv_instance_deactivate(instance);
      lilv_instance_free(instance);
    }
  }

  const std::string &uri() const noexcept override {
    return pluginUri;
  }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfos;
  }
  uint32_t latencySamples() const noexcept override {
    return publishedLatency.load(std::memory_order_acquire);
  }
  bool latencyRefreshPending() const noexcept override {
    return observedLatency.load(std::memory_order_acquire) !=
           publishedLatency.load(std::memory_order_acquire);
  }
  bool processingFailed() const noexcept override {
    return latencyInvalid.load(std::memory_order_acquire) != 0;
  }
  const std::string &failureIdentifier() const noexcept override {
    return pluginUri;
  }
  void latchProcessingFailure() noexcept override {
    latencyInvalid.store(1, std::memory_order_release);
  }
  bool refreshPluginLatency() override {
    const uint32_t updated = observedLatency.load(std::memory_order_acquire);
    return publishedLatency.exchange(updated, std::memory_order_acq_rel) !=
           updated;
  }

  const std::string &pluginIdentifier() const noexcept override {
    return pluginUri;
  }

  void setParameterValue(const std::string &symbol, float value) override {
    if (!std::isfinite(value))
      throw std::runtime_error("LV2 parameter value must be finite");
    size_t match = liveParameters.size();
    for (size_t i = 0; i < liveParameters.size(); ++i) {
      const auto &parameter = *liveParameters[i];
      if (symbol == parameter.symbol || symbol == parameter.name) {
        if (match != liveParameters.size())
          throw std::runtime_error("ambiguous LV2 parameter '" + symbol +
                                   "' (use its port symbol)");
        match = i;
      }
    }
    if (match == liveParameters.size())
      throw std::runtime_error("unknown LV2 input parameter '" + symbol + "'");
    auto &parameter = *liveParameters[match];
    if (value < parameter.minimum || value > parameter.maximum)
      throw std::runtime_error("LV2 parameter '" + symbol +
                               "' value is outside its declared range");
    // Control threads publish only to this lock-free mailbox. The audio
    // thread copies the newest value into the LV2 control-port storage at the
    // start of a process block, before calling run(); plugin port memory is
    // therefore never concurrently written by the control thread.
    parameter.value.store(value, std::memory_order_release);
  }

  bool savePersistentPluginState() override {
    if (!instance || !stateInterface || persistentIdentity.empty())
      return false;
    if (persistentIdentity.size() > 4096)
      throw std::runtime_error("LV2 state identity exceeds 4096 bytes");
    // Save is called on the quiesced control thread. Flush latest live
    // parameter mailboxes so an update issued just before graph replacement or
    // shutdown is not lost merely because no subsequent audio block ran.
    for (const auto &parameter : liveParameters)
      ports[parameter->portIndex].control =
          parameter->value.load(std::memory_order_acquire);
    auto &mapper = uridMapper();
    LilvState *state = lilv_state_new_from_instance(
        pluginRef, instance, &mapper.map, nullptr, nullptr, nullptr, nullptr,
        getStatePort, this, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features);
    if (!state)
      throw std::runtime_error("LV2 plugin failed to provide state: " +
                               pluginUri);
    lilv_state_set_label(state, persistentIdentity.c_str());
    std::ostringstream uri;
    uri << "urn:skyapo:lv2-state:" << std::hex << stateHash(persistentIdentity);
    char *serialized =
        lilv_state_to_string(worldRef, &mapper.map, &mapper.unmap, state,
                             uri.str().c_str(), nullptr);
    lilv_state_free(state);
    if (!serialized)
      throw std::runtime_error("cannot serialize LV2 plugin state: " +
                               pluginUri);
    std::string data(serialized);
    lilv_free(serialized);
    atomicWriteState(savedStatePath, data);
    return true;
  }

  std::vector<std::wstring>
  initialize(float, unsigned,
             const std::vector<std::wstring> &channels) override {
    return channels;
  }

  void process(float **output, float **input,
               unsigned frames) noexcept override {
    if (!instance || frames > maxFrameCount ||
        latencyInvalid.load(std::memory_order_acquire) != 0) {
      // Never expose stale samples when the host violates the negotiated
      // block bound. The CLAP and VST3 backends fail closed the same way.
      for (unsigned channel = 0; channel < channelCount; ++channel)
        std::fill_n(output[channel], frames, 0.0f);
      return;
    }
    for (const auto &port : ports) {
      if (port.kind == PortKind::AudioInput)
        lilv_instance_connect_port(instance, port.index, input[port.channel]);
      else if (port.kind == PortKind::AudioOutput)
        lilv_instance_connect_port(instance, port.index, output[port.channel]);
    }
    for (const auto &parameter : liveParameters)
      ports[parameter->portIndex].control =
          parameter->value.load(std::memory_order_acquire);
    struct AudioThreadScope {
      bool previous{UridMapper::audioThread};
      AudioThreadScope() {
        UridMapper::audioThread = true;
      }
      ~AudioThreadScope() {
        UridMapper::audioThread = previous;
      }
    } audioThreadScope;
    lilv_instance_run(instance, frames);
    if (latencyPortIndex >= 0) {
      uint32_t reported{};
      if (!readLatency(reported)) {
        latencyInvalid.store(1, std::memory_order_release);
        for (unsigned channel = 0; channel < channelCount; ++channel)
          std::fill_n(output[channel], frames, 0.0f);
      } else {
        observedLatency.store(reported, std::memory_order_release);
      }
    }
  }

private:
  bool readLatency(uint32_t &result) const noexcept {
    const float value = ports[static_cast<size_t>(latencyPortIndex)].control;
    constexpr uint32_t MaxLatencySamples = 1000000;
    if (!std::isfinite(value) || value < 0.0f || std::floor(value) != value ||
        value > static_cast<float>(MaxLatencySamples))
      return false;
    result = static_cast<uint32_t>(value);
    return true;
  }
  static const void *getStatePort(const char *symbol, void *opaque,
                                  uint32_t *size, uint32_t *type) {
    auto &self = *static_cast<LV2Instance *>(opaque);
    const auto found = std::find_if(
        self.ports.begin(), self.ports.end(), [&](const Port &port) {
          return port.controlInput && port.symbol == symbol;
        });
    if (found == self.ports.end())
      return nullptr;
    *size = sizeof(float);
    *type = uridMapper().map.map(uridMapper().map.handle, LV2_ATOM__Float);
    return &found->control;
  }
  static void setStatePort(const char *symbol, void *opaque, const void *value,
                           uint32_t size, uint32_t type) {
    auto &self = *static_cast<LV2Instance *>(opaque);
    const auto floatType =
        uridMapper().map.map(uridMapper().map.handle, LV2_ATOM__Float);
    if (size != sizeof(float) || type != floatType)
      return;
    const auto found = std::find_if(
        self.ports.begin(), self.ports.end(), [&](const Port &port) {
          return port.controlInput && port.symbol == symbol;
        });
    if (found == self.ports.end())
      return;
    const float restored = *static_cast<const float *>(value);
    if (!std::isfinite(restored))
      return;
    found->control = restored;
    for (auto &parameter : self.liveParameters)
      if (parameter->portIndex == found->index)
        parameter->value.store(restored, std::memory_order_relaxed);
  }
  static void cleanupNodes(LilvNode *a, LilvNode *b, LilvNode *c, LilvNode *d,
                           LilvNode *e, LilvNode *f, LilvNode *g) {
    lilv_node_free(a);
    lilv_node_free(b);
    lilv_node_free(c);
    lilv_node_free(d);
    lilv_node_free(e);
    lilv_node_free(f);
    lilv_node_free(g);
  }

  std::string pluginUri;
  LilvWorld *worldRef{};
  const LilvPlugin *pluginRef{};
  unsigned maxFrameCount;
  unsigned channelCount;
  int32_t latencyPortIndex{-1};
  std::atomic<uint32_t> observedLatency{0};
  std::atomic<uint32_t> publishedLatency{0};
  std::atomic<uint32_t> latencyInvalid{0};
  std::vector<Port> ports;
  LilvInstance *instance{};
  bool active = false;
  std::string persistentIdentity;
  fs::path savedStatePath;
  const LV2_State_Interface *stateInterface{};
  LV2_Feature mapFeature{};
  LV2_Feature unmapFeature{};
  const LV2_Feature *features[3]{};
  std::vector<PluginParameterInfo> parameterInfos;
  // Stable heap-owned atomic mailboxes are allocated during plugin setup;
  // neither publication nor consumption allocates or locks in the callback.
  std::vector<std::unique_ptr<LiveParameter>> liveParameters;
};

class LV2PluginFilter final : public IFilter,
                              public AtomicPluginBypass,
                              public IPluginLatencyState,
                              public IPluginLatencyRefresh,
                              public IPluginFailureState,
                              public IPluginParameterControl,
                              public IPluginStatePersistence {
public:
  LV2PluginFilter(LV2PluginHost &host, std::string uri,
                  std::vector<PluginParameterValue> parameters,
                  std::filesystem::path source = {}, unsigned line = 0)
      : host(host), pluginUri(std::move(uri)),
        parameterOverrides(std::move(parameters)), source(std::move(source)),
        line(line) {}

  bool getInPlace() override {
    return false;
  }

  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrameCount,
             std::vector<std::wstring> channelNames) override {
    channelCount = static_cast<unsigned>(channelNames.size());
    instance = source.empty()
                   ? host.create(pluginUri, sampleRate, maxFrameCount,
                                 channelNames, parameterOverrides)
                   : host.createForConfig(pluginUri, sampleRate, maxFrameCount,
                                          channelNames, parameterOverrides,
                                          source, line);
    auto outputChannels =
        instance->initialize(sampleRate, maxFrameCount, channelNames);
    prepareBypassDelay(instance->latencySamples(), channelCount);
    return outputChannels;
  }

  void process(float **output, float **input, unsigned frames) override {
    if (processingFailed()) {
      for (unsigned channel = 0; channel < channelCount; ++channel)
        std::fill_n(output[channel], frames, 0.0f);
      return;
    }
    if (copyInputWhenBypassed(output, input, frames, channelCount))
      return;
    instance->process(output, input, frames);
  }
  uint32_t latencySamples() const noexcept override {
    return instance && !processingFailed() ? instance->latencySamples() : 0;
  }
  bool latencyRefreshPending() const noexcept override {
    const auto *refresh =
        dynamic_cast<const IPluginLatencyRefresh *>(instance.get());
    return refresh && refresh->latencyRefreshPending();
  }
  bool refreshPluginLatency() override {
    auto *refresh = dynamic_cast<IPluginLatencyRefresh *>(instance.get());
    if (!refresh || !refresh->refreshPluginLatency())
      return false;
    const auto latency = instance->latencySamples();
    if (latency > skyapo::plugin::MaxRealtimeLatencySamples) {
      latchProcessingFailure();
      // Keep the daemon alive: this instance is silenced and reports zero
      // latency, then Engine rebuilds downstream compensation off-thread.
      return true;
    }
    prepareBypassDelay(latency, channelCount);
    return true;
  }
  bool processingFailed() const noexcept override {
    return instance && instance->processingFailed();
  }
  void latchProcessingFailure() noexcept override {
    if (auto *failure = dynamic_cast<IPluginFailureState *>(instance.get()))
      failure->latchProcessingFailure();
  }
  const std::string &failureIdentifier() const noexcept override {
    return pluginUri;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return pluginUri;
  }
  void setParameterValue(const std::string &symbol, float value) override {
    if (!instance)
      throw std::runtime_error("LV2 plugin is not active: " + pluginUri);
    auto *control = dynamic_cast<IPluginParameterControl *>(instance.get());
    if (!control)
      throw std::runtime_error("LV2 plugin does not support live parameters: " +
                               pluginUri);
    control->setParameterValue(symbol, value);
  }
  bool savePersistentPluginState() override {
    auto *state = dynamic_cast<IPluginStatePersistence *>(instance.get());
    return state && state->savePersistentPluginState();
  }

private:
  LV2PluginHost &host;
  std::string pluginUri;
  std::vector<PluginParameterValue> parameterOverrides;
  std::filesystem::path source;
  unsigned line{};
  std::unique_ptr<IPluginInstance> instance;
  unsigned channelCount{};
};

IFilter *allocatePluginFilter(LV2PluginHost &host, std::string uri,
                              std::vector<PluginParameterValue> parameters,
                              std::filesystem::path source = {},
                              unsigned line = 0) {
  void *memory = MemoryHelper::alloc(sizeof(LV2PluginFilter));
  try {
    return new (memory) LV2PluginFilter(
        host, std::move(uri), std::move(parameters), std::move(source), line);
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
                      const std::vector<std::wstring> &channels,
                      const std::vector<PluginParameterValue> &parameters) {
  return createForConfig(uri, sampleRate, maxFrames, channels, parameters, {},
                         0);
}

std::unique_ptr<IPluginInstance> LV2PluginHost::createForConfig(
    const std::string &uri, float sampleRate, unsigned maxFrames,
    const std::vector<std::wstring> &channels,
    const std::vector<PluginParameterValue> &parameters,
    const std::filesystem::path &source, unsigned line) {
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
  const auto identity =
      source.empty()
          ? std::string{}
          : stateIdentity(std::filesystem::absolute(source).lexically_normal(),
                          line, uri);
  return std::make_unique<LV2Instance>(world, plugin, uri, sampleRate,
                                       maxFrames, channels, parameters,
                                       identity);
}

PluginDescription LV2PluginHost::describe(const std::string &uri) const {
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

  PluginDescription result;
  result.uri = uri;
  LilvNode *name = lilv_plugin_get_name(plugin);
  result.name = name ? lilv_node_as_string(name) : "(unnamed)";
  lilv_node_free(name);

  LilvNode *controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
  LilvNode *inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
  if (!controlPort || !inputPort) {
    lilv_node_free(controlPort);
    lilv_node_free(inputPort);
    throw std::runtime_error("cannot create LV2 control-port metadata URIs");
  }
  for (uint32_t i = 0; i < lilv_plugin_get_num_ports(plugin); ++i) {
    const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
    if (!lilv_port_is_a(plugin, port, controlPort))
      continue;
    const bool input = inputPort && lilv_port_is_a(plugin, port, inputPort);
    if (!input)
      continue;
    const LilvNode *symbol = lilv_port_get_symbol(plugin, port);
    LilvNode *portName = lilv_port_get_name(plugin, port);
    LilvNode *def = nullptr, *minimum = nullptr, *maximum = nullptr;
    lilv_port_get_range(plugin, port, &def, &minimum, &maximum);
    const auto number = [](const LilvNode *value, float fallback) {
      if (value && lilv_node_is_float(value))
        return lilv_node_as_float(value);
      if (value && lilv_node_is_int(value))
        return static_cast<float>(lilv_node_as_int(value));
      return fallback;
    };
    const std::string symbolText = symbol ? lilv_node_as_string(symbol) : "";
    result.inputParameters.push_back(
        {symbolText, portName ? lilv_node_as_string(portName) : symbolText,
         number(def, 0.0f),
         number(minimum, -std::numeric_limits<float>::infinity()),
         number(maximum, std::numeric_limits<float>::infinity()),
         number(def, 0.0f)});
    lilv_node_free(portName);
    lilv_node_free(def);
    lilv_node_free(minimum);
    lilv_node_free(maximum);
  }
  lilv_node_free(controlPort);
  lilv_node_free(inputPort);
  return result;
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

class LV2PluginFilterFactory final : public IFilterFactory,
                                     public IPluginSourceContext {
public:
  LV2PluginFilterFactory() : host(std::make_unique<LV2PluginHost>()) {}

  void setPluginSourceLocation(const std::filesystem::path &path,
                               unsigned sourceLine) override {
    source = path;
    line = sourceLine;
  }

  std::vector<IFilter *> createFilter(const std::wstring &,
                                      std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, wideUri;
    input >> format >> wideUri;
    if (format != L"LV2")
      return {};
    if (wideUri.empty())
      throw std::runtime_error(
          "expected Plugin: LV2 <plugin-URI> [symbol=value ...]");
    const auto uri = StringHelper::toString(wideUri, 65001);
    std::vector<PluginParameterValue> overrides;
    std::wstring token;
    while (input >> token) {
      const auto separator = token.find(L'=');
      if (separator == std::wstring::npos || separator == 0 ||
          separator + 1 == token.size())
        throw std::runtime_error("expected LV2 parameter as symbol=value");
      const auto symbol =
          StringHelper::toString(token.substr(0, separator), 65001);
      const auto valueText =
          StringHelper::toString(token.substr(separator + 1), 65001);
      std::size_t consumed = 0;
      float value = 0.0f;
      try {
        value = std::stof(valueText, &consumed);
      } catch (const std::exception &) {
        throw std::runtime_error("invalid LV2 parameter value for '" + symbol +
                                 "'");
      }
      if (consumed != valueText.size() || !std::isfinite(value))
        throw std::runtime_error("invalid LV2 parameter value for '" + symbol +
                                 "'");
      if (std::any_of(
              overrides.begin(), overrides.end(),
              [&](const auto &entry) { return entry.symbol == symbol; }))
        throw std::runtime_error("duplicate LV2 parameter override '" + symbol +
                                 "'");
      overrides.push_back({symbol, value});
    }
    return {
        allocatePluginFilter(*host, uri, std::move(overrides), source, line)};
  }

private:
  std::unique_ptr<LV2PluginHost> host;
  std::filesystem::path source;
  unsigned line{};
};

std::unique_ptr<IFilterFactory> makeLV2PluginFilterFactory() {
  return std::make_unique<LV2PluginFilterFactory>();
}
