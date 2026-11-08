# LinuxAPO incremental release roadmap

LinuxAPO's published baseline is **v0.9.0 Beta**. Its tag and release are
immutable. This file is the canonical release sequence; the root
[`ROADMAP.md`](../ROADMAP.md) retains the detailed historical acceptance and
test-evidence log. Milestones below are planned gates, not claims of completion
or release dates. Releases in the 0.x series are GitHub prereleases.

## Release metadata and automation

For each release, create a distinct `docs/releases/vX.Y.Z.md` before tagging.
Start the draft from the actual commit subjects and changed paths since the
previous tag with:

```sh
python3 scripts/release_metadata.py draft 0.9.1 --topic "Supported-path correctness and release hygiene"
```

The version and topic above are examples only, not a preselected next release.
Choose the version from the verified change set using the policy below; the
topic must describe the actual release contents.

The generated file is deliberately a draft: review its source-history summary,
rewrite it for users, and replace the required test/limitations prompts with
evidence from the final release commit. Include only verified features, fixes,
compatibility statements, and contributor acknowledgements. Before tagging,
update the version-specific changelog entry, the README's released-version
links, `CMakeLists.txt`, and affected user/developer docs in the same final
release-preparation commit. Annotated tags use that release file's exact
first-level title followed by a concise release summary. Do not generate or
commit documentation from a tag-triggered workflow.

`.github/workflows/release.yml` is the single tag-triggered publisher. It
requires an annotated `v0.x.y` tag matching CMake, a matching changelog entry,
committed version-specific notes, and verified build/test/package jobs. The
same validator is unit-tested in ordinary CI and rejects duplicate generic
release topics, missing evidence sections, unfinished prompts, or a missing
changelog entry before any build jobs run. The tag annotation's title must
match the release-note title and include a summary. It
creates a draft prerelease only after all jobs pass, downloads the uploaded
assets back, verifies `SHA256SUMS`, and only then makes the prerelease public.
The workflow rejects an existing release and does not run for v1.0.0. The
publish job alone has `contents: write`; development commits continue through
ordinary CI without producing releases.

### Change-driven 0.x version selection

The planned milestone headings below organize engineering work; they do not
require finishing an entire milestone before publishing useful work. Choose a
patch version (`0.x.Y`) for compatible fixes, stability/compatibility work and
small refinements; choose a minor version (`0.X.0`) when a coherent, verified
set of changes adds meaningful functionality. Do not wait for a predetermined
number of commits, and do not tag every commit. Each prerelease still needs a
clean release commit, matching CMake version, unique reviewed notes, required
CI/package gates and verified checksums. The release workflow creates only
0.x prereleases; v1.0.0 remains an explicit user-approval gate.

## Baseline and current checkpoint

The 0.9.0 baseline provides an upstream-backed EAPO DSP path from PipeWire
capture to the `SkyAPO Virtual Mic`, offline rendering, CLI/daemon and a Qt
editor. LinuxAPO also contains native LV2, CLAP and single-audio-bus VST3
hosts; FST-based VST2 compatibility is optional and experimental. This is not
full EAPO parity, system-wide playback processing, or production-final.

The active unpublished checkpoint is **v0.10.0 PipeWire playback** on
`feature/v0.10-playback-candidate`; it remains a prerelease candidate, not a
published version. Current playback commits include `fc2e167` (native input
adapter), `0e8b3af` (exact existing consumer-link adoption), `0c61223` (native
playback controls frontend), and `944ace4` (WirePlumber 0.4 PortConfig
compatibility and E2E fixture correction). The playback frontend uses a
native adapter and keeps the upstream DSP in a hidden fixed-port graph.
Release native-controls/lifecycle CTest passed 1/1, including eight
native-reference controls cases, readiness without a producer, silence through
adapter recreation and nonuniform gain restoration. After the restoration-ack
gate, a focused sequential ASan/UBSan run passed **9/9** (153.86 s): dump
parser, stereo/mono fixtures, playback tests #46/#47, quantum variation #50,
output switch #60, PDC recovery #91, and native controls/lifecycle #103.
PipeWire subprocess tests ran with the existing
`ASAN_OPTIONS=detect_leaks=0`; this is not LeakSanitizer evidence or a full
sanitizer suite.

The `944ace4` follow-up was reproduced against WirePlumber 0.4.17 before
changing any assertions. The virtual source now refreshes its fixed output
ports while paused after a `PortConfig` request, allowing WirePlumber's
`si-audio-adapter` to complete activation. The playback-isolation fixture now
passes the target's actual `object.serial` to `target.object` (WirePlumber 0.4
matches that property by serial) and starts an ordinary stream only after the
default sink is confirmed as `skyapo.playback`. No expected values or
tolerances were relaxed. The private 0.4.17 graph passed: app → SkyAPO playback
node → DSP stream → selected sink; playback node ID 53, DSP stream ID 63,
target ID 39, 2/2 internal links and 2/2 target-channel links. The independent
recording measured `0.999111` of expected output after the configured
`-6 dB` Preamp; status reported DSP amplitude ratio `0.501187`, zero callback
allocations/deallocations and zero overruns. The explicitly targeted separate
sink measured `0.995706` of unity, and the longest near-silence interval during
link recovery was `0.021 ms` against a `<250 ms` limit. CTest #46 and #47
passed 1/1 each (18.38 s and 18.50 s); #48 passed on host WirePlumber 1.6.10
(35.71 s). A complete local Release build passed at `--parallel 1`, as did
clang-format 18, Python syntax, and `git diff --check`.

On exact commit `944ace4`, hosted formatting, release metadata and ASan/UBSan
checks passed; hosted `build-and-test` and Arch packaging were still running
at the time of this checkpoint. These virtual-graph checks do not prove
physical output playback: the workstation had an active EasyEffects route,
Firefox and Discord streams, so no live route was changed. Physical output,
hotplug, ordinary-client selection and hardware diversity remain open.

WirePlumber default-target fixture correction (2026-11-08; follow-up to
`548866e`): local reproduction showed test #56 processed audio through the
playback graph and recovered its selected sink, but its shutdown assertion
failed. The fixture initialized `default.audio.sink` with `pw-metadata` alone;
that bypassed WirePlumber's own default-target state. When the test sink was
removed, WirePlumber selected the alternate sink and re-applied that fallback
after `skyapod` had correctly restored the original PipeWire metadata. The
fixture now selects the initial sink with `wpctl set-default` and verifies the
metadata before starting the daemon. Existing shutdown expectations and audio
thresholds are unchanged. On the local PipeWire 1.6.10/WirePlumber 0.5.18
environment, focused CTest #48, #56 and #57 passed **3/3** sequentially
(59.08 s): ordinary application routing, target removal/recreation, and stable
device identity. Playback RMS ratios were 0.999113, 0.999996 and 1.000004;
unrelated playback measured 0.993585 at unity. The virtual-microphone capture
checks also passed, and callback allocation/deallocation counters were 0/0.
This changes only the integration fixture and does not resolve the other
failures from the hosted 101-test run on `548866e`; no full local or sanitizer
suite was run for this test-only change. Physical-device validation remains
open.

The historical entries below retain the test scope recorded at each checkpoint. New focused PipeWire E2E
tests for LV2 and VST3 state persistence across a fresh daemon restart passed
1/1 each in Release and ASan/UBSan. Earlier full suites passed **76/76**
Release and **77/77** ASan/UBSan. Newer evidence supersedes those counts: full
Release CTest passed **83/83** on 2026-10-15, before the CLI daemon-stop probe
race fix; after that fix, the two affected Release plugin E2Es passed **2/2**
on 2026-10-16. The rebuilt current-source ASan/UBSan suite then passed
**84/84** sequentially on 2026-10-16 (`ASAN_OPTIONS=detect_leaks=0`; the
PipeWire subprocess suite excludes LeakSanitizer). After fixing an
asynchronous Device Selector readiness race in the UI editor test, the latest
complete ASan/UBSan CTest run passed **87/87** sequentially on 2026-10-16 with
`ASAN_OPTIONS=detect_leaks=1` (487.09 s). The UI test waits for both fixture
tables and daemon streaming status before asserting device details. Headless
Qt uses the `compose` input method; narrowly scoped LeakSanitizer suppressions
cover Fontconfig/FreeType process-global caches. The latest full sanitizer
run includes 51 private PipeWire E2Es. At that earlier post-UI-adjustment
checkpoint, full Release CTest passed **86/86** sequentially (445.38 s; 50
PipeWire E2Es). Release and sanitizer builds intentionally differ in optional
test configuration: Release enables the systemd crash-restart E2E and disables
the optional FST/VST2 host; ASan enables FST/VST2 live-parameter and
dynamic-latency E2Es and disables the `systemd-run` E2E. Those historical
86/86 and 87/87 runs covered their respective configured matrices, not
identical test lists. Current-source
DEB, RPM and generic `.tar.zst` extracted-payload smokes and `-6 dB` renderer
checks also passed. These are local dirty-worktree results, not hosted CI on a
clean commit or hardware diversity validation; CI must retest the exact
checkpoint commit before a release.

Current dirty-worktree checkpoint (2026-10-17): after a fresh CMake configure,
the complete Release build succeeded at one job and CTest passed **96/96**
sequentially in 658.27 s, including 58 private PipeWire E2Es. The changed
Device Selector verification passed Release UI tests **2/2**, its focused
ASan/UBSan test **1/1**, and a live private PipeWire/WirePlumber output-switch
E2E **1/1**. A disk-backed staged install passed both
`package_smoke_test.py` and the extracted-payload renderer test. Fresh CPack
DEB and RPM packages and the generic `.tar.zst` were each extracted and passed
the package smoke checks; these temporary artifacts were removed and are not
release assets. The Arch pacman install/remove transaction was not run: the
PKGBUILD correctly refuses this dirty source tree, and the host Docker daemon
is unavailable. The full ASan/UBSan matrix subsequently passed **98/98**
sequentially in 764.07 s with `ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`; it includes 60 private
PipeWire E2Es. Compared with Release, the sanitizer configuration additionally
enables two experimental VST2 fixture E2Es; this is not third-party Windows
VST compatibility evidence. No physical microphone or speaker test was
performed. The source still declares version 0.9.0, so these checks do not
constitute a candidate release or hosted-CI verification.

Legal/package hardening follow-up (2026-10-16): the owner-supplied branded app
artwork is now excluded by default from Qt resources, the generated desktop
entry and installed PNGs. CMake refuses the explicit opt-in unless the
license audit records `CLEARED` and all eight sizes exist; release automation
accepts either `CLEARED` or `EXCLUDED` only when excluded assets are not Git
tracked. Release CMake/UI metadata/resource/license tests passed **4/4**;
a fresh disk-backed `DESTDIR` install passed package smoke and contained no
branded PNGs. The negative opt-in configuration failed with the expected
rights-clearance diagnostic. `release.yml` parsed with PyYAML, its EXCLUDED
guard passed locally, and `git diff --check` passed; `actionlint` was not
installed. Rebuilt `skyapod` and capture plus playback PDC recovery E2Es passed
**2/2**. These focused checks do not replace a full Release/sanitizer run,
hosted CI, or physical-output/hotplug validation.

Latest focused reload correction (2026-10-16): an invalid `post-mix` branch
could previously be rejected by playback after the capture Engine had already
been installed, while the CLI still reported a successful reload. Runtime now
reserves capture retirement capacity, validates/commits playback first, and
publishes capture only after playback accepts the candidate. The WirePlumber
stage E2E injects a capture-valid/playback-invalid config, verifies the reload
fails while capture remains at unity and an independent sink-monitor recording
retains the prior −6 dB playback response, then verifies a valid stage-specific
reload updates both graphs. The `skyapod` incremental Release build passed;
focused Release CTest passed **7/7** across the stage/reload, core,
pre-mix-warning, direct playback, Include reload, Delay/PDC, and LV2 PDC recovery
tests. Local formatting, Python syntax, and `git diff --check` passed. This is
private PipeWire evidence on a dirty worktree, not a full post-change suite,
sanitizer run, clean-commit CI result, or physical-hardware validation.

Subsequent full Release verification (2026-10-16): `cmake --build build
--parallel 1` passed. Sequential CTest ran **87** tests: **86 passed**, and
`skyapo-config-check-json` failed because its fixture still expected a warning
for `Stage: post-mix`, which is now supported. The fixture was corrected to
assert the warning for unsupported `Stage: pre-mix`; the isolated corrected
test passed **1/1**. All other tests in the full run passed, including the new
stage/reload E2E, 51 PipeWire E2Es, plugin/PDC recovery and systemd restart.
The full matrix has not been rerun after this test-expectation-only correction,
so this is not recorded as a single-run 87/87 pass. The updated stage/reload
E2E was also built and passed **1/1** in the ASan/UBSan tree; the existing
PipeWire subprocess policy disables LeakSanitizer for that case. The full
sanitizer matrix has not been rerun after the Runtime reload change.

Later full Release candidate regression (2026-10-16): after all currently
configured Release targets were rebuilt with `nice -n 15 ionice -c 3 cmake
--build build --parallel 1`, sequential `ctest --test-dir build
--output-on-failure --parallel 1` passed **91/91** in 542.94 seconds. This
includes 54 private PipeWire E2Es: capture format cases, playback/WirePlumber
routing and multichannel, device switch/recovery, daemon restart, plugin
failure/state/latency and PDC recovery, plus renderer/config/UI/package checks.
The most recent complete ASan/UBSan matrix remains **87/87** from an earlier
same-day candidate; the full matrix was not rerun on the exact current tree.
Focused current-source PDC core, realtime-safety, capture recovery and playback
recovery tests passed **4/4** under ASan/UBSan. The sanitizer build's optional
FST/VST2/systemd test configuration differs from Release. These results are
from dirty local worktrees, not hosted CI on a clean commit. The full Release
run is from HEAD `64efe9935734f5471d003da8669320d014cd9ed4`.
Audible-level physical playback and physical device churn remain unverified.

Dynamic-latency fail-closed follow-up (2026-10-16): a live LV2 fixture exposed
audio escaping on the first block where plugin latency exceeded the PDC budget
(monitor RMS 1.8218 versus 0.03544 expected, 51.4×). Capture and playback
callbacks now inspect active and transitioning Engines before publishing each
processed chunk and zero the complete PipeWire quantum when latency becomes
unsafe. Capture always publishes `Quiescing` independently of the event
deduplication bit. Control-thread recovery drains callbacks before discarding
an unvalidated playback transition; audio remains faulted until a config
reload is validated. The playback E2E holds the transition open for 500 ms,
changes quantum, records continuously across the rebuild/reload boundary, and
checks fail-closed silence followed by valid `-6 dB` recovery. Focused Release
build and five tests passed (core PDC allocation failure, realtime-safety,
capture PDC recovery, playback latency refresh and playback PDC recovery).
Focused ASan/UBSan runs passed the same four relevant tests (core allocation,
realtime-safety, capture and playback recovery). `clang-format`, Python syntax,
and `git diff --check` passed at that focused checkpoint. The full dirty-tree
CTest rerun is recorded immediately below; clean-commit CI and broader
physical-device validation remain outstanding.

Full post-fail-closed candidate regression (2026-10-16; dirty local source at
HEAD `64efe9935734f5471d003da8669320d014cd9ed4`): rebuilt the Release tree
with `nice -n 15 ionice -c 3 cmake --build build --parallel 1`; build passed.
Sequential `ctest --test-dir build --output-on-failure --parallel 1` passed
**91/91** in 549.74 seconds, including **54** private PipeWire E2Es. Rebuilt
the ASan/UBSan tree at one job and ran sequential CTest with
`ASAN_OPTIONS=detect_leaks=1`; **92/92** passed in 593.02 seconds, including
**55** private PipeWire E2Es. CMake options differ between these trees:
Release includes the systemd crash-restart test and excludes optional FST/VST2,
while the sanitizer tree enables FST/VST2 tests and omits systemd-run. The
existing CTest policy disables LeakSanitizer only for PipeWire subprocess
tests; non-PipeWire tests ran with LeakSanitizer enabled. Build/test temporary
files remained on the disk-backed build trees. This closes the local full-suite
gap for the fail-closed candidate, but is not hosted CI on a clean commit or
physical-device diversity/audible-level validation. The existing paired
physical-sink result used a `-60 dBFS` signal and does not establish safe
audible-level desktop use or physical hotplug recovery.

Quantum-capacity follow-up and full candidate regression (2026-10-17; dirty
worktree at HEAD `64efe9935734f5471d003da8669320d014cd9ed4`): `Engine` now
accepts variable frame counts up to its prepared capacity unless a fixed-block
filter requires the exact configured block. Capture and playback callbacks
use that capability check instead of treating every smaller PipeWire quantum
as a format mismatch. The new WirePlumber quantum-variation E2E cycles
512/1024/256/1024 frames four times, checks that the playback Engine is not
needlessly rebuilt, and measures processed output with an independent
consumer. Incremental Release build (`--parallel 2`) passed; full sequential
Release CTest passed **93/93** in 560.01 s, including 55 private PipeWire
E2Es. The ASan/UBSan tree rebuilt at one job and its full sequential CTest
passed **94/94** in 616.86 s, including 56 private PipeWire E2Es. The
configured test matrices differ: Release includes systemd crash-restart,
while ASan/UBSan includes optional VST2 live-parameter and dynamic-latency
tests. Playback quantum variation, capture renegotiation and playback PDC
recovery also passed focused local reruns; the PDC recovery E2E passed in
both builds. Nix 2.35.2 `flake check`, package smoke and `skyapo` package
build also passed against a filtered 125 MiB source snapshot containing the
dirty and untracked candidate files; the snapshot excluded ignored build and
package output and was removed afterward. Changed-line formatting was checked
locally with clang-format 23.1.1, but the repository-pinned 18.1.3 executable
is unavailable here.
These are current dirty-worktree results, not a clean-commit hosted CI run,
physical playback/hotplug validation or completion of the remaining 1.0.0
gates.

Control-callback exception-boundary revalidation (2026-10-17; same dirty
worktree at HEAD `64efe9935734f5471d003da8669320d014cd9ed4`): the incremental
Release build passed and sequential CTest passed **93/93** in 559.83 s,
including 55 private PipeWire E2Es. This rerun includes the new
`invokeControlCallback` regression cases for successful callbacks,
`std::bad_alloc`, and non-standard exceptions, and confirms the existing
playback, PDC, recovery, plugin and systemd tests remain green. The focused
ASan/UBSan `skyapo-realtime-safety` test passed **1/1** in 12.52 s with
LeakSanitizer enabled. The full sanitizer matrix was not rerun after this
callback-boundary change; this test validates the exception helper and the
existing allocation audit, not exception injection through every PipeWire
control callback. Results are from the dirty local candidate, not hosted CI or
physical playback/hotplug validation.

Callback-recovery admission regression (2026-10-17; same dirty candidate):
`skyapo-realtime-safety` now also coordinates a live callback, control-thread
drain, Engine-swap window and a callback arriving after the gate closes. It
asserts that the in-flight callback drains before swap and that the late
callback is not admitted to Engine processing. The Release test passed 10
consecutive runs; the rebuilt ASan/UBSan test passed 1/1 with LeakSanitizer
enabled. Playback baseline plus capture and playback PDC-recovery PipeWire
E2Es passed 3/3 in Release. This is a deterministic gate/unit regression and
private-graph evidence, not physical playback, TSan, or clean-commit CI proof.
The gate also has compile-time assertions that its callback counter and closed
flag are always lock-free. After adding those guards, the Release focused group
passed 4/4 and ASan/UBSan passed the safety plus playback PDC-recovery pair
2/2; both builds compiled `skyapod` and the safety-test target.

Playback stream teardown ordering audit (2026-10-17): PipeWire 1.6.9 source
shows `pw_filter_disconnect()` deactivating/destroying the node, while removing
the process listener synchronizes with its data loop. `recreateStreamFilter()`
now removes that listener before changing port handles observed by its
realtime callback. The existing private WirePlumber output-switch E2E passed
1/1 before the change, then 3/3 Release repetitions and 1/1 ASan/UBSan after
it. No crash was reproduced before the change; this is defensive teardown
ordering based on PipeWire's synchronization path, not evidence of a crash
having occurred. It is not TSan or physical device-switch evidence.

Package-manager install/remove smoke (2026-10-16; dirty local candidate): fresh
DEB and RPM artifacts were generated from the current Release build. The DEB
was installed and removed with `dpkg --root` inside a `fakeroot` isolated root;
the RPM was installed and erased inside an unprivileged user namespace with its
RPM database rooted in a disposable directory. The staged install trees passed
`packaging/package_smoke_test.py` before removal, and the package-owned
`skyapo` executable was absent after each removal. The RPM check used
`--nodeps --noscripts --notriggers --nosignature`; the Debian check forced
dependency configuration because this Arch host has no Debian dependency
database. These are package-manager file ownership/removal checks, not clean
Debian/Fedora dependency-resolution or distro install validation. Docker was
unavailable, and no package was installed into the host system. The fresh
generic `.tar.zst` was also extracted and passed the same payload smoke; its
installer instructions remain manual by design. Package staging was cleaned
after successful runs; verification payloads were moved to Trash afterward.

Playback now has two levels of private-graph evidence. The direct-link E2E
connects a deterministic producer to the daemon's `SkyAPO Playback` node and
measures `Preamp: -6 dB` at 48 kHz stereo/Q1024 (RMS ratio 0.999956; 2/2
target links). More importantly, the opt-in WirePlumber E2E uses an ordinary
untargeted PipeWire playback stream, the real SPA `support.null-audio-sink`
adapter, and an independent client recording that sink's monitor ports.
WirePlumber routes the app stream through `SkyAPO Playback`; the recorded
monitor RMS ratio was 0.999997 of the expected -6 dB output, with internal and
target links both 2/2, zero callback allocations/deallocations, and zero
reported overruns. The playback nodes acknowledge WirePlumber's
`SPA_PARAM_PortConfig`/`Props` negotiation, and the stream advertises
`node.autoconnect` plus the selected-sink routing metadata.

This private-graph evidence demonstrates policy-based app routing and monitor
recording on the current PipeWire 1.6.9 / WirePlumber 0.5.18 environment; by
itself it is not physical-device validation. A separate physical sink smoke is
recorded below. The v0.10.0 milestone remains open until the candidate also
passes the documented restart/removal/configuration and capture-regression
gates, audible-level physical playback is confirmed, and ordinary desktop-app
routing is validated. Device diversity and desktop-session policy behavior
remain unverified.

Physical playback smoke (2026-10-16): on the current Arch desktop, SkyAPO
captured from built-in input node `alsa_input.pci-0000_00_1f.3.analog-stereo`
(PipeWire node 54) and exposed `SkyAPO Playback` node 68 to the selected
built-in output `alsa_output.pci-0000_00_1f.3.analog-stereo` (node 53). The
negotiated format was 48 kHz, stereo, quantum 1024; internal and physical
target links were both 2/2. An independent PipeWire client recorded the
physical sink's monitor ports while a deterministic `-60 dBFS` test stream
passed through the graph. With `Preamp: 0 dB`, recorded RMS was `0.00060579`;
with `Preamp: -6 dB`, it was `0.00030361`, a measured ratio of `0.501180`
against the expected `0.501187`. The system default sink remained node 53,
the daemon shut down cleanly, and no other audio streams were active before
the test. This proves signal traversal and relative DSP gain on one physical
PipeWire sink; it does not prove audible-level playback, ordinary desktop-app
routing while other streams are active, physical hotplug/restart recovery, or
device diversity. A preceding absolute-level assertion differed by about
9.4%; the paired A/B measurement avoids treating the sink-monitor path's
fixed level as DSP gain.

Post-change physical playback integration check (2026-10-16; dirty local
candidate at HEAD `64efe99`): with an isolated `XDG_CONFIG_HOME`, the daemon
captured from built-in input node 54 and processed an untargeted native
PipeWire test-app stream through `SkyAPO Playback` to built-in output node 53.
An independent PipeWire consumer captured the physical sink's monitor ports.
Both runs negotiated 48 kHz stereo/Q1024 with 2/2 playback target links. The
producer amplitude was `0.001` (`-60 dBFS`): monitor RMS was `0.00035187` for
`Preamp: -6 dB` and `0.00049880` for `Preamp: -3 dB`, ratio `1.417569` versus
the expected `1.412538` (0.356% relative error). The test stream had no explicit
target, so the measured config-dependent gain verifies ordinary PipeWire
session-policy routing through the DSP path on this host. Default sink node 53
and its 0.60 volume were unchanged; after daemon shutdown the virtual nodes and
streams disappeared, and `skyapo status` reported the daemon unreachable. The
test used a low-level tone, not audible-level validation; no unrelated stream
was active, and it does not prove feedback behavior with other applications,
physical hotplug/restart recovery, or device diversity. This is local evidence
from a dirty tree, not clean-commit hosted CI.

Playback control follow-up: both PipeWire playback-node `Props` callbacks now
apply mute and effective gains using lock-free atomic state. Ordinary
WirePlumber playback E2Es measured the expected −18 dB after two independent
0.5 controls and silence after stream mute; the baseline direct playback and
ordinary app route both still pass. Callbacks report zero audited allocations,
deallocations and overruns. This does not close physical output or device
recovery gates.

Playback target recovery now handles selected-sink removal and return on the
PipeWire registry control path: it invalidates old port/link IDs, rematches the
replacement by persisted serial/bus identity (or exact node name where no
stable identity exists), updates the WirePlumber smart target and waits for
the expected channel links before reporting ready. A private
PipeWire/WirePlumber E2E destroyed node 43 and recreated the same-name null
sink as node 51; a second independent monitor recording measured ratio
0.999987 to expected `Preamp: -6 dB`, with 2/2 playback target links restored.
The capture graph and daemon remained alive while the sink was absent. This
does not prove physical unplug/replug, renamed-device matching, or recovery
under different desktop session policies; those remain open for v0.10.0 and
the 1.0 hardware gate.

Playback multichannel follow-up (2026-10-16): new WirePlumber E2Es passed for
5.1 and 7.1 sink layouts (Release and ASan/UBSan, sequential, 1/1 each per
layout; LeakSanitizer is disabled for these PipeWire subprocess E2Es by the
existing test policy). Both run a multichannel ordinary PipeWire producer
through the real playback DSP and capture the selected null-sink monitor with
an independent consumer while the capture-to-virtual-mic graph remains active.
The 7.1 Release run negotiated 48 kHz,
8 channels, quantum 1024, restored all 8/8 internal and target links after a
deliberately removed link, and measured RMS ratio 0.999997 against expected
`Preamp: -6 dB`; callback allocations/deallocations were both zero. The 5.1
test also passed its independent-consumer gain and link assertions. These
private synthetic graph checks do not validate physical multichannel output,
hardware routing, or device diversity.

Focused UI/playback follow-up (2026-10-16): the Qt Device Selector lists
capture and playback endpoints from stable-name CLI inventories. Capture
selection restarts an active daemon; changing playback target while the daemon
runs now refreshes the existing graph in place, preserving the application to
smart-filter link. When the daemon is stopped, playback selection only saves
the preference and does not issue a restart. A UI regression test initially
expected an obsolete second restart; its assertion was corrected to verify no
extra restart, and `skyapo-ui-editor` then passed **1/1** (16.46 s).

The private PipeWire/WirePlumber output-switch E2E passed **1/1** (9.86 s): it
launched the real Qt Device Selector offscreen, selected the alternate sink
through the UI, confirmed the capture node stayed active and the ordinary
application stream stayed linked through the smart filter, then independently
recorded the processed stream (monitor RMS ratio to expected: 1.000019).
Playback node 62 remained active while target selection changed. Name-fallback
and stable-ID target recovery each passed
**1/1** after recovery-state reporting was cleared on return to streaming.
These are synthetic graph tests, not physical-device hotplug evidence. The
host's default daemon was unreachable during the private E2Es, so they do not
prove behavior under an ordinary active desktop session.

Transactional output-switch failure follow-up (2026-10-17): a test-only
PipeWire interposer now fails exactly one new playback-stream creation after
the Device Selector requests a different sink. The CLI reports the failed
switch, the persisted target remains the previous stable selection, and the
daemon rolls its PipeWire graph back to that sink while preserving the live
capture graph and an ordinary application stream. A separate recorder measured
the restored monitor output at `0.999988` of expected in Release and `0.999980`
under ASan/UBSan; four subsequent live sink round trips also remained at
`0.999996` and `1.000001`, respectively. Both configurations passed the
focused settings-plus-output-switch CTest pair **2/2** sequentially. Runtime
status reported the expected `-6 dB` DSP ratio (`0.501187`), zero callback
allocations/deallocations, and no overruns. The injected failure is confined
to the test process and does not add hooks to the installed daemon. This is a
private PipeWire/WirePlumber graph test, not proof of physical sink recovery,
audible-level playback safety, or recovery during real hardware hotplug.

Cross-stage config rollback while playback is unavailable (2026-10-17): the
startup-recovery E2E reproduced a real transaction gap before the fix: with
the selected playback target absent, an invalid `Stage: post-mix` directive
was accepted by the capture-only Engine, allowing capture to change before
playback later rejected the same config. `Runtime::reloadConfig()` now builds
and validates a temporary post-mix Engine on the control thread using the
current negotiated graph shape and last-known output identity before it
installs the capture replacement. The actual playback Engine is still rebuilt
against the returned sink's negotiated format and ports. After this change,
the playback-stage and startup-recovery PipeWire E2Es passed **2/2** in Release
and **2/2** under ASan/UBSan, sequentially. The invalid reload is rejected and
capture remains at its last-good `-6 dB`; the valid config is accepted after
the target returns. These are private PipeWire graph tests, not physical
hotplug or hardware evidence.

Device Selector verification follow-up (2026-10-17): the selector now offers
an asynchronous, read-only **Verify audio path…** report. It checks the saved
editor config with `skyapo config check` and requires its canonical path to
match the daemon's reported active `Config:` path; if editor changes are
unsaved, the dialog says they are excluded. It also checks daemon/DSP state,
selected capture availability, virtual-mic presence, capture links, playback
target availability, and playback links. It does not restart services, change
routes, or emit a signal; numerical DSP and physical hardware are explicitly
reported as NOT TESTED. The Qt fixture covers a healthy graph plus injected
config/DSP/link/node failures, and asserts only read-only CLI commands are
issued. `skyapo-ui` built, `skyapo-ui-device-selector-polkit` passed **1/1**,
and `skyapo-ui-editor` passed **1/1**. Full CTest and ASan/UBSan were not rerun;
this host had elevated interactive load, so the larger matrix is deferred.

The focused current-tree regression group (IPC, editor, playback DSP, ordinary
WirePlumber routing, volume, mute, both recovery modes and live target switch)
passed **9/9** sequentially in Release on 2026-10-16 (164.84 s). At that point
this was not a full CTest run; subsequent full Release 86/86 and ASan/UBSan
87/87 results are recorded in the current checkpoint at the top of this
document. A preceding ASan/UBSan build also passed the three most sensitive
playback lifecycle E2Es—same-name recovery, stable-ID recovery and in-place
output switch—**3/3** sequentially (29.42 s; `detect_leaks=0` for PipeWire
subprocess tests).

The first renamed-node/stable-identity E2E exposed that `device.serial` can be
published through `pw_node_info` rather than the registry global. Playback
now binds sink nodes for their info events and retains observed input-port
globals until identity matching completes. After this fix, both name-fallback
and serial-matched rename recovery E2Es pass; the renamed sink was independently
recorded at a 0.999989 ratio to expected `Preamp: -6 dB`. This is still
synthetic PipeWire evidence, not physical sink unplug/replug. The full focused
Release playback group passes 6/6; both target recovery cases pass ASan/UBSan
2/2 (`detect_leaks=0` for the PipeWire subprocess suite).

Full dirty-candidate verification (2026-10-17; source worktree at HEAD
`64efe9935734f5471d003da8669320d014cd9ed4`): reconfigured with
`cmake -S . -B build`, then rebuilt all targets using
`nice -n 15 ionice -c 3 cmake --build build --parallel 1` (PASS). Sequential
`nice -n 15 ionice -c 3 ctest --test-dir build --output-on-failure --parallel
1` passed **92/92** in 582.93 s, including 54 private PipeWire E2Es. The
existing `SKYAPO_SANITIZERS=ON` build tree passed a focused **13/13** ASan/UBSan
set covering core, realtime-safety, playback processing, quantum/stage changes,
target recovery/switching and playback PDC recovery (306.57 s, sequential;
CTest applies the repository's PipeWire LeakSanitizer policy). This is not a
full sanitizer matrix. `git diff --check` passed. Results are from the dirty
candidate, not a clean commit or hosted CI. The host had only 1.6 GiB available
memory afterward, so no full sanitizer matrix was started. No physical
playback, audible-level test or hardware unplug/replug was performed in this
run; those gates remain open.

Focused UI and playback revalidation (2026-11-05; dirty worktree): Analysis
response now reports the maximum finite calculated Engine magnitude as
`Peak gain (calculated)` and explicitly states that this is neither live
clipping nor a hardware measurement. Numerical UI tests cover a −6 dB preamp
and a combined −6/+6 dB response; editor assertions check the saved default
and current document values. Reproduced an ASan UI-test failure caused by the
host's inherited `QT_QPA_PLATFORMTHEME=kde`: Qt's KDE platform theme retained
notification/dialog objects at process exit, causing LeakSanitizer to report
1,570 bytes in Qt/KF6. Setting the headless UI CTest environment to the
`generic` platform theme made the same test pass with LeakSanitizer enabled;
both UI tests now use that deterministic theme. Release UI editor passed
1/1; ASan/UBSan UI editor and Device Selector/Polkit tests passed 1/1 each.
Four private PipeWire playback E2Es passed 4/4: Preamp processing, 5.1 channel
routing, stable-ID target recovery, and playback PDC recovery. `git diff
--check` passed. These focused tests do not replace a fresh full-suite run,
hosted CI, or physical playback/device-switch validation; the live desktop
route was left unchanged.

Full candidate regression (2026-11-06; dirty worktree at base HEAD
`64efe9935734f5471d003da8669320d014cd9ed4`): the incremental Release build
passed with one compile job, then serial CTest passed **98/98** in 672.18 s,
including 60 private PipeWire E2Es and 17 playback cases. The ASan/UBSan build
also passed with one compile job; serial CTest passed **100/100** in 755.88 s
with `ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, including 62 private
PipeWire E2Es and the opt-in VST2 prototype cases. The Release and sanitizer
configurations intentionally contain different test lists. No test or
sanitizer failure was reported. These results apply to this dirty local source
tree, not a clean commit or hosted CI. Private PipeWire playback/recovery
passed, but the desktop's only present physical sink is muted and the
configured USB sink is absent; no physical playback route or volume was
changed, so audible-level playback, ordinary-app hardware routing, and physical
device hotplug remain unverified. A fresh `DESTDIR` install then passed
`packaging/package_smoke_test.py` and
`packaging/container_fresh_install_test.py --root`, including offscreen UI
startup, installed CLI/config validation, and a WAV renderer check confirming
the `Preamp: -6 dB` gain and WAV format. The successful build-local staging was
removed; this did not exercise a real pacman install/remove transaction.

### Current hardening workstream — feeds the v0.10.0 candidate

**Purpose:** complete correctness and release-hygiene work that accompanies
the already implemented playback feature. Because the current candidate
contains substantial playback functionality, it will not be mislabeled as a
v0.9.1 patch release.

**Scope:** bounded memory and transactional PDC/reconfiguration; actionable
fail-closed behavior for runtime/plugin errors; DSP/config boundary checks;
repeatable callback, latency and state-restart tests; disk-backed temporary
staging; package/install smoke corrections. Optional experimental hosts stay
disabled in default packages. The unverified application artwork currently in
the worktree is excluded from public commit/package/release until its
redistribution rights are confirmed; the rest of the candidate must not depend
on shipping it.

**Acceptance criteria:**

- Full Release CTest and ASan/UBSan suites pass on the exact candidate commit;
  no current supported-path P0/P1 defect remains.
- PipeWire independent-consumer E2Es verify processed audio, config rollback,
  source/server recovery, supported channel/rate transitions, and plugin
  state/reload paths. Runtime callback allocation/deallocation counters remain
  zero in steady state.
- Renderer/config numerical tests verify Preamp, filter, convolution, delay,
  channel routing and invalid-config rollback wherever implemented.
- Package smoke covers every artifact being published, with install paths,
  desktop metadata, service files and notices verified. Unsupported package
  families are not advertised as validated.
- `git status` is clean at the release commit; source, submodules and licenses
  are audited; no unlicensed artwork, generated binaries, cache or secrets are
  included.

**Required tests:** full CTest; ASan/UBSan; PipeWire producer→daemon→independent
consumer; renderer/config; plugin fixtures and state restart; callback
allocation invariants; PDC/latency tests; package install/remove smoke; Nix
checks if its derivation changes. Hosted CI must pass on the release commit.

**Hardware/external validation:** physical capture-to-virtual-mic smoke and
physical playback validation are required for the candidate described below.
Broader device diversity and independent third-party plugin compatibility
remain unproven and are not advertised as covered by fixture tests.

**Known limitations:** physical playback and wider hardware diversity are not
yet verified; arbitrary third-party plugin crashes/hangs are not isolated;
total ADC-to-application latency and several distribution install matrices
are not proven. FST/VST2 remains experimental and subject to legal review.

**Release readiness:** this workstream is not a separately tagged milestone.
It feeds the v0.10.0 candidate below, which remains unpublished until all
applicable gates pass on a reviewed, clean commit. Application artwork may be
included only after redistribution rights are confirmed; otherwise its source
assets must remain outside the commit and every package, with default builds
omitting the Qt resource, desktop icon reference, and installed PNGs.

### v0.10.0 Beta: PipeWire playback processing

**Scope:** add a native PipeWire playback graph for selected application audio,
process it with the same EAPO-backed engine, and deliver it to a selected
physical sink. Keep capture-to-virtual-microphone as an independent supported
route. Specify graph ownership, default/fallback behavior and loop/feedback
prevention before implementation.

**Worktree progress (2026-10-16 through 2026-10-17; uncommitted):** the EAPO expression variable
`stage` and `Stage:` command now resolve independently for each Linux graph:
capture uses `capture`, while the PipeWire graph processing the mixed playback
stream uses `post-mix`. Core numerical tests cover both expression and command
selection. A private WirePlumber E2E measured playback `Preamp: -6 dB` at RMS
ratio `0.999997` to expected while the capture path remained unity (`1.000041`);
callback allocations/deallocations and overruns were zero. The editor exposes
`capture` and `post-mix`; unsupported per-application `pre-mix` stays disabled
with an explanation and warning. On 2026-10-17, an internal DSP→stream playback
link recovery test was extended to cover a PipeWire `pw_proxy` asynchronous
error (invalid output port), not only a synchronous link-factory refusal or
global-link removal. The runtime now listens for proxy errors and retries on
the PipeWire control loop with bounded backoff. Focused Release tests passed
4/4: realtime safety, ordinary link removal/refusal recovery, asynchronous
proxy-error recovery, and output-switch/rollback. After rebuilding the daemon
under ASan/UBSan, focused CTest passed **3/3**: realtime safety, ordinary link
removal/refusal recovery and asynchronous proxy-error recovery. The
output-switch/rollback case and full sanitizer matrix remain untested under
sanitizers in this increment; subsequent heavy local testing was deferred as
memory pressure and swap use rose during the E2Es. Follow-up on 2026-10-17
added a separate two-second independent sink-monitor capture after the
asynchronous link recovery status cleared; focused Release and ASan/UBSan CTest
each passed **1/1**, measuring output/expected gain ratios **0.994646** and
**0.994640**, respectively, for `Preamp: -6 dB`. This proves audio was flowing
after recovery, not uninterrupted audio during the link fault. The full
sanitizer matrix was not rerun after this test-only change.
Physical playback and unplug/replug remain unverified.
Playback PipeWire-server restart was verified in the private graph on
2026-10-17. The first run reproduced a real test-coverage gap: capture
reconnected while the selected playback sink was unavailable. The E2E now
restarts WirePlumber and its test sink, waits for both playback link pairs,
then records the sink monitor during an ordinary playback stream. Release and
ASan/UBSan each passed the new test **1/1**; the broader related recovery sets
passed **4/4 Release** and **3/3 ASan/UBSan**. The post-restart sink-monitor
RMS ratio was **0.991431** to the expected `Preamp: -6 dB` output, within the
3% tolerance. This proves synthetic service/graph recovery and actual output
after recovery, not continuous audio while PipeWire is offline, physical
hardware restart, or audible-level desktop behavior.
Playback systemd crash-recovery coverage was added on 2026-10-17 as an
opt-in test separate from the capture-only crash test. It SIGKILLs the
transient user-service daemon, waits for systemd to start a new PID and for
both playback link pairs to return, then plays a fresh ordinary stream and
records the sink monitor independently. Release passed the new test **1/1**
and the related PipeWire-server, capture-systemd and playback-systemd
recovery group **3/3**; ASan/UBSan passed the playback-systemd test **1/1**.
Measured post-crash sink-monitor RMS ratio: **0.991431** to expected
`Preamp: -6 dB`, within 3%. This covers playback after supervised crash
recovery in a synthetic graph; uninterrupted audio across process death and
physical output behavior are not proven.
Playback internal-link recovery now also measures the longest consecutive
near-silence run at the independent sink-monitor consumer, starting after its
first non-silent sample so graph startup does not count as a dropout. Both the
ordinary recovery and asynchronous proxy-error E2Es passed in Release (**2/2**);
the async-error E2E also passed in ASan/UBSan (**1/1**). The test enforces a
250 ms maximum and measured **0.021 ms** in Release and ASan/UBSan while the
post-recovery recording matched the expected `-6 dB` output (latest ratios
**1.000041** and **0.999999**). Callback allocation/deallocation counters were
0/0 and reported overruns were zero. This is a sample-level measurement in the
private PipeWire graph, not acoustic/physical playback evidence or a guarantee
for other schedulers/devices.
This source state is not yet a reviewed/committed release candidate and does
not satisfy physical playback acceptance.

Capture config-commit recovery hardening (2026-10-17): a config reload now
re-checks plugin-latency, callback-fault and queued-validation state only after
draining callbacks at the commit boundary. A queued PDC event keeps the audio
gate quiesced, skips saving potentially inconsistent live plugin state, and
requires candidate validation before a hard swap. If validation or the paired
playback commit fails, the old capture state resumes only when no fault/event
still requires silence. A deterministic PipeWire E2E now pauses the control
thread immediately after candidate construction, activates the live LV2 plugin
to increase latency, and keeps an already-connected independent consumer
recording through the barrier. On 2026-10-17 the test passed **1/1** in the
Release build: measured silence RMS **0.00000000**, then recovered `-6 dB`
ratio **0.999951** at 96 kHz / 1024 frames; status reported zero callback
allocations/deallocations and zero overruns. After the final owner-reservation
adjustment, the focused Release and ASan/UBSan subsets each passed **5/5**:
realtime safety, capture/playback PDC recovery, Include reload and
playback-stage transaction. The callback-exception E2E passed **1/1** before
that control-thread-only adjustment. After test-client cleanup hardening, the
same Release E2E was rerun on 2026-10-17 and passed **1/1** in 8.33 seconds;
the focused sanitizer rerun remains deferred until local memory pressure
subsides.

**Acceptance criteria:**

- A native PipeWire test producer's known signal passes through LinuxAPO DSP
  and is captured by an independent consumer attached to the intended physical
  sink/monitor path; numeric gain/filter output matches reference tolerances.
- UI/CLI/status report selected sink, graph state, negotiated format and
  unavailable/unsupported playback states without fabricated values.
- Capture mode remains regression-clean; selecting playback cannot create a
  capture/playback feedback loop or silently redirect unrelated streams.
- Graph teardown, daemon restart, PipeWire restart, device removal and config
  reload fail safely and recover when the endpoint returns.
- No recurring realtime heap allocation; bounded buffers and measured graph
  latency are reported honestly.

**Required tests:** private PipeWire producer→LinuxAPO→sink/monitor→independent
consumer E2E; unity and non-unity DSP numeric checks; routing/feedback,
format/quantum changes, daemon and server restart, capture regression,
allocation, sanitizer and latency tests.

**Hardware/external validation:** verify audible playback on at least one real
physical output and capture a loopback/monitor recording. Verify no feedback
under ordinary desktop playback. Other audio servers and device diversity are
not implied.

**Latest playback-routing verification (2026-11-06):** the PipeWire backend now
publishes a regular virtual sink and temporarily selects it as the PipeWire
default, allowing ordinary default-target application streams to pass through
SkyAPO. It no longer relies on WirePlumber 0.5 Smart Filters. On the local
WirePlumber 0.5.18 private graph, the default playback E2E and `link-error`,
`stage`, and `volume` variants passed **4/4**; the independent consumer
verified `Preamp: -6 dB`, zero callback allocations/deallocations and zero
overruns. The tests verify restoration of the previous default on clean stop
and verify a `wpctl set-default` change relinquishes SkyAPO's route ownership
and remains selected after shutdown. Hosted build/test and Arch package jobs
for `ca2c920` were still running at this checkpoint; hosted sanitizer and
release-metadata jobs passed. Commit `cf9144f` adds the external-default
ownership/status regression and has local build/E2E verification, but awaits
hosted CI.

**Known limitations:** playback routing temporarily changes the per-user
PipeWire default while SkyAPO runs. Local WirePlumber 0.5.18 verifies that
behavior, including respecting a user-selected replacement default. CI for
WirePlumber 0.4.17 is still required to establish compatibility with the
older policy used by Ubuntu 24.04. Current hardware inspection found only the
built-in analog sink, which is muted; the configured USB headphone sink is
absent. Physical audible playback/loopback is therefore unverified and was
not attempted. Per-application routing, simultaneous multi-sink graphs and
JACK/PulseAudio-specific behavior remain out of scope unless testing exposes
a need.

**Release readiness:** publish only after real playback and independent
recording evidence exists and capture/virtual-mic acceptance remains green.

### v0.11.0 Beta: endpoint selection and lifecycle reliability

**Scope:** close user-facing gaps in input/output device identity, switching,
hotplug, default-device changes and PipeWire reconnect/recovery across capture
and playback routes.

**Acceptance criteria:** stable identifiers survive runtime numeric-ID churn;
selection changes are transactional; removed endpoints fail visibly and
recover without stale links, hangs or silent routing to a different device.
UI/CLI and daemon status agree on current endpoint and graph state.

**Required tests:** private PipeWire endpoint add/remove/rename/default-change,
same-name reappearance, server restart, selected-device switch during audio,
capture and playback independent-consumer checks, UI/CLI persistence and
sanitizer tests.

**Hardware/external validation:** real microphone and speaker unplug/replug or
equivalent device disable/enable on available hardware. Synthetic hotplug is
not a substitute for physical validation.

**Known limitations:** device-specific APO installation/registry behavior has
no literal Linux analogue; LinuxAPO follows PipeWire routing and permissions.

**Release readiness:** no unresolved route-loss/reconnect regression; publish
only with at least one documented physical endpoint-switch recovery run.

### v0.12.0 Beta: supported plugin workflow and compatibility

**Scope:** define and test the supported native plugin matrix (LV2, CLAP,
VST3); improve state, parameter, bypass, failure and latency/PDC UX. Decide
separately whether a native plugin GUI is required; do not claim one until it
actually loads and safely controls plugins. Keep FST/VST2 opt-in/experimental
unless compatibility and legal gates are met.

**Acceptance criteria:** per-format discover/load/parameter/state/bypass/reload
and latency behavior is documented; unsupported buses/features fail
actionably; saved state survives clean daemon restart; malformed state rolls
back to last valid graph; no host callback allocation; plugin failure does not
silently corrupt audio/config state.

**Required tests:** native fixture and selected real third-party plugins for
each claimed format, offline numeric output, PipeWire independent-consumer
E2E, restart/corrupt-state, dynamic PDC, block-size changes, callback
allocation and ASan/UBSan. Third-party coverage must name plugin/version.

**Hardware/external validation:** monitor/record at least one native plugin
chain through capture and playback on hardware. Wine/yabridge wrappers are
separately identified and never treated as native-plugin evidence.

**Known limitations:** plugin crashes/hangs remain in-process unless an
isolation milestone is explicitly implemented; automation curves, auxiliary
buses and GUI embedding may remain unsupported.

**Release readiness:** publish only the formats that meet the per-format gate;
exclude experimental VST2 from default packages and marketing claims.

### v0.13.0 Beta: EAPO config compatibility and analysis

**Scope:** prioritize high-value missing EAPO commands and semantics from
[`PORT_GAP_AUDIT.md`](PORT_GAP_AUDIT.md), particularly stages/device matching,
expressions/runtime variables, nested includes and channel/mix behavior; finish
useful magnitude/phase/latency response analysis. No Windows COM/Registry
replica.

**Acceptance criteria:** each claimed parser command has syntax/units/alias,
source-location diagnostics, runtime wiring and numerical or routing tests;
unsupported commands fail visibly; editor writes valid semantics and
round-trips untouched directives; response analysis has independent reference
fixtures and distinguishes modeled response from measured realtime latency.

**Required tests:** upstream-config corpus comparison; parser/error/Include
matrix; mono/stereo/multichannel numeric DSP and routing tests; UI
round-trips; response magnitude/phase fixtures; PipeWire consumer tests for
commands that affect routing/audio; renderer regression and sanitizers.

**Hardware/external validation:** sample-rate and channel-layout checks on
available physical devices. External configs/plugins require a documented,
permission-safe fixture corpus.

**Known limitations:** full Windows EAPO parity is neither assumed nor
required; unsupported Windows-specific commands remain explicitly listed.

**Release readiness:** no command may be called supported based only on parse
success; runtime effect and tests must be shown. Publish with an updated parity
audit and no silent command loss.

### v0.14.0 Beta: packaging, diagnostics and cross-distro hardening

**Scope:** stabilize supported package formats and upgrade/removal behavior,
systemd user lifecycle, diagnostics, docs, reproducible artifacts and
checksums; close reliability issues found across prior milestones.

**Acceptance criteria:** fresh install/upgrade/remove for every declared
package family; service start/restart/disable/remove semantics verified;
installed UI/CLI/docs/notices match source; packages build from a clean pinned
checkout; no generated/user plugin/cache material is bundled.

**Required tests:** clean build; CTest and sanitizer suites; package smoke and
transaction tests per declared distro; Nix flake check/build; artifact
checksums; startup/recovery and uninstall-no-leftover tests; hosted CI on exact
candidate commit.

**Hardware/external validation:** repeat capture and playback smoke after
install on the supported desktop. Other distros are only supported if their
package transaction was actually exercised.

**Known limitations:** distro versions beyond the tested matrix, unsupported
init systems, secure-boot/sandbox integration and third-party plugin sandboxing
are not implied.

**Release readiness:** package and repository state are clean, provenance and
licenses are documented, and all advertised artifacts pass their own smoke
tests.

### v1.0.0: stable release gate

**Scope:** freeze the supported hardware, PipeWire, config, plugin and
distribution matrix; resolve or explicitly exclude every mandatory gap; make
no new feature additions during the release candidate phase.

**Acceptance criteria:**

- System-wide playback and capture-to-virtual-mic meet their separate E2E and
  physical-device gates; switching/recovery is validated on real hardware.
- Supported EAPO syntax and plugin formats are explicitly defined; no
  supported command parses but is disconnected from realtime/offline output.
- No known critical crash, hang, data loss, unbounded memory or realtime
  allocation issue in supported use; last-valid-config rollback works.
- Full Release, ASan/UBSan, PipeWire, renderer/config, plugin, restart/recovery,
  PDC/latency, allocation and package suites pass on the final source commit.
- Clean install/upgrade/uninstall works for each declared distro/package;
  Nix check/build passes on the pinned source.
- GPL/upstream notices, plugin/SDK licensing and all redistributed artwork
  have release-specific clearance; no user-provided third-party plugin or
  yabridge/Wine payload is bundled.
- Independent user-facing documentation and final artifact hashes match the
  tagged commit.

**Hardware/external validation:** representative real capture and playback
devices, at least one physical switch/reconnect, third-party plugins for every
claimed plugin format, declared distro transaction matrix, and qualified
license review. Unavailable mandatory gates mean stop at the latest beta.

**Known limitations:** only the frozen support matrix is promised; literal
Windows APO/COM/Registry/service behavior is not a goal. Peace Equalizer
compatibility is not claimed without a dedicated integration test.

**Release readiness:** tag v1.0.0 only from a clean reviewed commit after every
mandatory criterion has attached evidence. Do not waive blockers by relabeling
them optional.

## Beyond 1.0 (optional)

Plugin subprocess isolation, automation curves, auxiliary plugin buses, wider
response/impulse analysis, additional package channels, broader hardware
certification and a potential Peace Equalizer backend integration are optional
until explicitly promoted into the stable support contract.
