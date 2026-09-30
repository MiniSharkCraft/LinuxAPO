# CLAP plugin state: implementation blocker

CLAP state persistence is not implemented yet. The pinned CLAP API provides
`clap_plugin_state_t` (`clap.state`) with stream-based `save()` and `load()`
callbacks, but SkyAPO does not yet have a safe end-to-end state lifecycle.

The immediate blocker is synchronization, not stream serialization:

- `CLAPInstance` in `src/plugin/CLAPPluginHost.cpp` initializes, activates,
  and starts processing a plugin during graph construction.
- `CLAPInstance::process()` is subsequently called by the realtime audio
  callback, while config/control work runs on the PipeWire main loop.
- CLAP specifies state `save()` and `load()` as main-thread callbacks. These
  callbacks are not marked thread-safe, so the host must not assume that calling
  `save()` on the active plugin can safely overlap `process()`. A mutex in the
  audio callback is not an acceptable fix because it could block realtime
  processing.
- `Runtime` already counts in-flight callbacks to defer destruction of retired
  engines, but there is no operation that quiesces the active graph and gives
  the control thread exclusive access to plugin lifecycle/state methods.
- Graph construction currently has no per-plugin persistent state input/store.
  Restore itself can be placed before `activate()`, but a durable state format,
  stable instance key, bounded stream handling, transactional load failure
  behavior, and save trigger/lifecycle still need to be integrated with the
  engine/daemon.

The required safe sequence for a later implementation is: on the control
thread stop publishing audio work to the target graph, wait until its in-flight
callback count reaches zero, stop/deactivate the CLAP instance as required by
the lifecycle operation, then save/load through bounded CLAP streams. Restore must occur after plugin
`init()` and before `activate()`. Resume processing only after a successful
state operation (or retain/reinstall the last valid graph if restoration
fails). State file IO and plugin state callbacks must remain off the realtime
thread. The serialized bytes should be written atomically to a stable,
versioned per-instance store rather than to an ephemeral PipeWire node ID.

Relevant pinned API declarations:

- `upstream/clap/include/clap/ext/state.h`: state save/load are
  `[main-thread]` callbacks.
- `upstream/clap/include/clap/stream.h`: stream callbacks can transfer partial
  data and require loops that handle short reads/writes and errors.
- `src/pipewire/Runtime.cpp`: `callbacksInFlight`, `installEngine()`, and the
  realtime `process()` callback currently provide deferred reclamation, not a
  state-operation barrier.

No CLAP state save/restore claim should be made until that lifecycle and an
end-to-end state round-trip test exist. The existing CLAP parameter overrides
and live parameter updates are not plugin-state persistence.
