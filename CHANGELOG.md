# Changelog

## Unreleased

- Added portable recursive `Include:` expansion with relative paths, quoted paths, nesting/cycle guards, and file/line errors.
- Config loads remain transactional: a parse or include failure does not replace the active filter list.
- Added debounced inotify reload; candidate graphs are built on the control loop, atomically activated, and old graphs are reclaimed only after audio callbacks leave them.
- Added nested-include, sample-output, error-location, cycle, and last-valid-config tests.
- Added the staged 0.2.0 through 1.0.0 roadmap and acceptance criteria.

## 0.1.0 — realtime prototype baseline

- Pinned official SourceForge Equalizer APO source as a clean submodule.
- Built actual upstream Preamp, BiQuad, IIR and Delay DSP for Linux.
- Added offline WAV rendering, native PipeWire device selection/capture/virtual source, daemon status, allocation checks and realtime numerical proof.
- This is a development baseline, not a stable 1.0 release.
