# Troubleshooting

- **No device list / cannot connect:** confirm the per-user PipeWire and WirePlumber session is running. `skyapo device list` uses native registry enumeration.
- **No physical input selected:** run `skyapo device set <live ID or node.name>`; the stable node name is persisted. Runtime numeric IDs may change between sessions.
- **Daemon will not start:** read stderr, run `skyapo config check FILE`, inspect `skyapo device current`, and check whether another daemon owns the runtime lock.
- **Virtual source is missing:** inspect `skyapo status`, `wpctl status`, and `pw-dump`; daemon reconnects after source/selected device loss. Current tested profile is stereo float32 at 48 kHz.
- **Client records the physical microphone instead:** explicitly select `SkyAPO Virtual Mic` in the application's input list. The manual recording probe sets no-fallback/no-move properties and independently checks -6 dB.
- **Config edit not audible:** check the daemon log and `skyapo status` for a reload error; the watcher observes edits/replacements in the config directory. Unsupported directives fail and report their source file/line; on failure the last valid graph remains active.
- **LeakSanitizer reports PipeWire allocations:** compare with `skyapo-pipewire-lifetime`; the current environment reproduces module/context teardown allocations without SkyAPO DSP. CTest's own leak-enabled suite passes.
