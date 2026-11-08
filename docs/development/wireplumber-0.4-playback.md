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
