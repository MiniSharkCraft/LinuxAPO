# Architecture

The CLI and daemon communicate over a per-user Unix-domain stream socket at `$XDG_RUNTIME_DIR/skyapo.sock`, mode 0600. Protocol v1 requests are `SKYAPO/1 STATUS\n`, `SKYAPO/1 RELOAD\n`, or `SKYAPO/1 STOP\n`; the client half-closes its write side after sending. Responses use `SKYAPO/1 OK <byte-length>\n<payload>` or `SKYAPO/1 ERR <byte-length>\n<payload>`, followed by EOF. Requests are limited to 128 bytes and response payloads to 4 MiB. The control loop rejects legacy, unknown-version, malformed, oversized and unknown-command requests before dispatch; control reads/writes have a one-second deadline. Status payload text remains the CLI's existing public output and includes the active filter directive/source-line list, negotiated audio state, timing, overrun and allocation metrics. A socket that accepts a connection but fails to return status before the client timeout is reported as unresponsive; `skyapo status` exits nonzero, while diagnostics retain build/runtime version data and report the timeout state. `skyapo diagnostics --json` converts current status into typed machine-readable fields and uses JSON `null` for unavailable measurements. Diagnostics include the compiled SkyAPO version, exact upstream revision, and linked PipeWire library version (or report that PipeWire was not built). This IPC is served by the PipeWire control loop, never by the audio process callback. CLI-launched daemons detach from the terminal and append logs to `$XDG_CONFIG_HOME/skyapo/skyapod.log` (or the XDG fallback config directory).

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
