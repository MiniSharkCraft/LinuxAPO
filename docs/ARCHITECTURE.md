# Architecture

SkyAPO runs control/config work in `skyapod`'s main loop and audio processing in a PipeWire filter process callback. The physical source is linked to planar float filter inputs; channel audio enters the platform-neutral `Engine`, then leaves through the PipeWire virtual source `skyapo.virtual_mic`. CLI communicates status over a private user-session Unix socket. Device identity is persisted by stable PipeWire node name.

`Engine` currently owns actual upstream `IFilter` implementations but its config reader/orchestration is a Linux adapter, not upstream `FilterEngine` or `FilterConfiguration`. Current commands are Preamp, parametric/IIR Filter, Delay and nested Include. Config file changes arrive through directory inotify and are debounced on the PipeWire control loop. A complete candidate engine is constructed before an atomic active-pointer switch; old engines remain owned until the callback in-flight counter reaches zero, then the control loop destroys them. Parse errors leave the active engine alone and appear in status.

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
