# WirePlumber 0.4 playback activation investigation

Status: unresolved; do not interpret the experiments below as a product fix.
Reference candidate: `48a8f2c`; investigation recorded 2026-11-08.

## Reproduction and evidence

Hosted run [42435608592](https://github.com/MiniSharkCraft/LinuxAPO/actions/runs/42435608592)
passed 85/100 tests. Fifteen WirePlumber playback cases failed before ordinary
application audio reached the DSP. Direct playback tests #46/#47 passed.
Sanitizer, Arch package and fresh-root install/remove jobs passed separately.
The graph had DSP-to-stream and stream-to-target links, but no application-to-DSP
link. Setting default-sink metadata succeeded and did not establish that route.

The same failure reproduces locally using WirePlumber 0.4.17 with the host's
PipeWire 1.6.10. This is not an exact Ubuntu dependency-stack reproduction.
A private PipeWire graph and private D-Bus session are used; no desktop routing
is intentionally changed. The default 0.4 configuration also starts hardware
monitors, so subsequent isolated experiments should disable those monitors.

## Source-level findings

WirePlumber `src/scripts/policy-node.lua::checkPending()` suspends linking while
any session item is pending activation. Instrumentation identified runtime
playback, stream and virtual-mic nodes, plus raw-filter test source/consumer
nodes, in this state. `modules/module-si-audio-adapter.c` sends `PortConfig`
and completes its task on `on_node_ports_changed` or port parameter information.
`lib/wp/node.c` forwards its port object manager's `objects-changed` signal;
`lib/wp/object-manager.c` emits that signal after object addition/removal.
LinuxAPO currently echoes node parameters and updates node Props, which does
not provide that port-list transaction.

## Controlled experiments (scratch only)

- Calling `on_node_ports_changed` immediately after the adapter's configuration
  request made the playback E2E pass: output/expected 0.999110, DSP gain
  0.501187, audited callback allocations/deallocations 0/0. This changes the
  policy's timing contract and is diagnostic evidence only.
- Marking raw nodes `item.features.no-format` did not restore playback.
- Selecting `si-node` for raw nodes did not restore playback.

No experiment changed LinuxAPO assertions or its tracked implementation.
The scratch dependency needed GCC's pointer-type diagnostic downgraded from
error to warning; LinuxAPO compiler flags were not changed.

## Required next validation

Implement a supported adapter/port-configuration contract and run against an
unmodified WirePlumber 0.4.17. Preserve fixed buffer ownership and callback
lifetimes; do not create dummy ports or repeatedly rebuild active ports merely
to generate an event. Retain ordinary application autoconnection, numeric DSP,
explicit-target isolation, capture continuity, recovery and zero-allocation
assertions. Retest WirePlumber 0.5 and the actual Ubuntu CI stack before claiming
compatibility. Physical playback and hardware recovery remain separate gates.

## Native frontend controls contract (candidate, not yet released)

A native null-sink frontend with `monitor.channel-volumes=false` can feed the
existing DSP at unity while its node Props supply post-DSP controls. Private
reference measurements also read back Props after every request to rule out
ignored setters. With `monitor.channel-volumes=true`, the reference monitor
applies `channelVolumes` and `mute`; changing scalar `volume` alone stores the
value without changing monitor gain. The candidate must reproduce this measured
contract, not infer the last request's contents from full Props snapshots.

The previous raw filter accepted scalar-only requests as a gain fanout. That
low-level behavior cannot be reconstructed reliably from native snapshots:
a repeated channel-volume request may produce an unchanged snapshot despite
requiring a different gain under the old request-precedence rule. The initial
snapshot-diff heuristic demonstrably retained 0.8 gain after a repeated 0.5
channel request. It must not be shipped as preserving the previous API.

The proposed native frontend therefore uses channel volumes and mute as the
authoritative audio controls, with scalar volume retained as native metadata.
Clients that directly sent scalar-only Props to the old raw node must send
`channelVolumes` to change gain. Public controls remain after DSP so nonlinear
filters retain their gain ordering; the output stream's separate controls
remain separate. This is an explicit compatibility correction for the new
frontend, not a claim of complete old raw-node API equivalence.

A mandatory regression compares native reference and candidate recordings
through reset, scalar-only changes, channel changes, repeated identical channel
requests, mute/unmute and nonuniform stereo gains. Keep existing amplitude and
silence tolerances. Scalar-only reference gains remain unity; uniform channel
0.5 yields 0.5; channels [0.5, 0.25] yield pooled RMS gain approximately
0.395285. Candidate expectations additionally include the actual -6 dB DSP.
The earlier scratch measurement script intentionally reported nonunity cases
without failing globally; it is diagnostic evidence, not a release gate.
