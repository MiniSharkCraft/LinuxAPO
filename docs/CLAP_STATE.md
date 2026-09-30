# CLAP plugin state persistence

CLAP state persistence uses the pinned CLAP API's
`clap_plugin_state_t` (`clap.state`) with stream-based `save()` and `load()`
callbacks. SkyAPO persists supported plugin state in a per-directive sidecar
and restores it before activation.

The realtime integration uses a short graph-quiescence window:

- `CLAPInstance` in `src/plugin/CLAPPluginHost.cpp` initializes, activates,
  and starts processing a plugin during graph construction.
- `CLAPInstance::process()` is subsequently called by the realtime audio
  callback, while config/control work runs on the PipeWire main loop.
- CLAP specifies state `save()` and `load()` as main-thread callbacks. These
  callbacks are not marked thread-safe, so the host must not assume that calling
  `save()` on the active plugin can safely overlap `process()`. A mutex in the
  audio callback is not an acceptable fix because it could block realtime
  processing.
- `Runtime` atomically publishes a null Engine pointer and waits for the
  in-flight callback count to reach zero before saving the active Engine. During
  this brief window, the PipeWire callback outputs silence; it performs no
  waiting or state operations. The Engine is republished after save succeeds or
  fails. A control-operation drain has a 500 ms timeout; on timeout SkyAPO
  republishes the Engine and refuses the save/reload rather than racing plugin
  processing.
- `CLAPInstance` calls `stop_processing()` on the exclusive symbolic audio
  thread before `save()`; it flushes pending parameter events while quiescent,
  calls the state extension on the main thread, writes the bounded payload to a
  mode-0600 sidecar via fsync + atomic rename, then restarts processing.
- State identity is the canonical config source path, directive line, and CLAP
  plugin ID. Sidecars live under `$XDG_STATE_HOME/skyapo/clap-state`, falling
  back to `$HOME/.local/state/skyapo/clap-state`. The versioned envelope checks
  identity and an FNV-1a corruption checksum; state payloads are capped at
  16 MiB. Missing files mean plugin defaults. Corrupt/mismatched sidecars and
  plugin `load()` rejection fail candidate graph creation; Engine's transactional
  config load keeps its previous graph.
- The daemon saves before config/rate graph rebuilds and on explicit shutdown
  after disconnecting/destroying the PipeWire filter and draining callbacks.
  State callbacks and sidecar file IO never run in the realtime callback or a
  plugin/Engine destructor.

The remaining limitation is that the brief mute window is audible, and the
sidecar checksum detects accidental corruption but is not cryptographic
authentication. At shutdown, SkyAPO destroys/disconnects the PipeWire filter
before draining callbacks. If the drain exceeds 500 ms, it skips state saving
but continues waiting until callbacks finish before allowing Engine teardown;
a plugin callback that never returns can therefore delay shutdown indefinitely.
The state fixture checks main-thread/non-audio-thread calls, exact payload
bytes, numerical gain round-trip, missing/corrupt state, plugin load rejection,
and transactional retention of the old graph. A private PipeWire E2E also
verifies state across a live graph reload and daemon restart, and checks that a
corrupt sidecar leaves the old audio graph consumable.

Relevant pinned API declarations:

- `upstream/clap/include/clap/ext/state.h`: state save/load are
  `[main-thread]` callbacks.
- `upstream/clap/include/clap/stream.h`: stream callbacks can transfer partial
  data and require loops that handle short reads/writes and errors.
- `src/pipewire/Runtime.cpp`: `callbacksInFlight`, `installEngine()`, and the
  realtime `process()` callback currently provide deferred reclamation, not a
  state-operation barrier.

The existing CLAP parameter overrides remain config-controlled; ordinary live
parameter updates become durable only when the plugin implements `clap.state`
and the daemon completes a successful graph rebuild or orderly shutdown.
