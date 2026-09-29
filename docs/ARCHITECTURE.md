# Architecture

The CLI and daemon communicate over a per-user Unix-domain stream socket at `$XDG_RUNTIME_DIR/skyapo.sock`, mode 0600. The current line-oriented requests are `STATUS`, `RELOAD`, and `STOP`; a request receives one text response and then EOF. Status contains the active filter directive/source-line list, negotiated audio state, timing, overrun and allocation metrics. `skyapo diagnostics` prefixes this with the compiled SkyAPO version and exact upstream revision. This IPC is served by the PipeWire control loop, never by the audio process callback. CLI-launched daemons detach from the terminal and append logs to `$XDG_CONFIG_HOME/skyapo/skyapod.log` (or the XDG fallback config directory). The protocol is local and intentionally small; use a versioned framing format before adding external clients or structured responses.

SkyAPO runs control/config work in `skyapod`'s main loop and audio processing in a PipeWire filter process callback. The physical source is linked to planar float filter inputs; channel audio enters the platform-neutral `Engine`, then leaves through the PipeWire virtual source `skyapo.virtual_mic`. CLI communicates status over a private user-session Unix socket. Device identity is persisted by stable PipeWire node name.

`Engine` currently owns actual upstream `IFilter` implementations but its config reader/orchestration is a Linux adapter, not upstream `FilterEngine` or `FilterConfiguration`. Current commands are Preamp, parametric/IIR Filter, Delay, Channel, Copy and nested Include. A Linux `ChannelHelper` provides the portable subset of upstream channel-name lookup needed by the reused Channel/Copy filters. Config file changes arrive through directory inotify and are debounced on the PipeWire control loop. A complete candidate engine is constructed before an atomic active-pointer switch; old engines remain owned until the callback in-flight counter reaches zero, then the control loop destroys them. Parse errors leave the active engine alone and appear in status.

The callback receives preallocated per-channel ports and work buffers. It does not parse configuration, enumerate devices, perform filesystem I/O, log synchronously, or allocate through the audited executable allocation calls. New graphs must be prepared on the control side and retired there, never freed from the audio callback. PipeWire and SPA types stay in `src/pipewire`; upstream DSP sees float channels and Equalizer APO channel names.

```text
CLI ── Unix status/control socket ── skyapod main/control loop
                                         │
Physical PipeWire source ── links ── Engine (upstream filters)
                                         │
                                  virtual source node
                                         │
                                  ordinary clients
```

See [REALTIME](REALTIME.md) for the verified session, graph IDs, sample measurements, instrumentation boundaries and sanitizer caveat.
