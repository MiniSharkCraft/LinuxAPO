# Low-impact local development policy

LinuxAPO is developed on a daily-use desktop. Keep local builds and tests
responsive and leave resources available for interactive applications.

- Default to at most two compile jobs (`--parallel 2` / `-j2`); use one for
  memory-heavy builds or whenever the desktop feels slow. Do not run build
  configurations concurrently.
- Start local builds and tests at low CPU priority, for example:
  `nice -n 15 ionice -c 3 cmake --build build --parallel 2` and
  `nice -n 15 ionice -c 3 ctest --test-dir build --output-on-failure --parallel 1`.
  If `ionice` is unavailable, use `nice` alone.
- Check `free -h`, `df -h /tmp .`, and system load before expensive work.
  Avoid concurrent memory-heavy tests; run ASan/UBSan sequentially. Prefer CI
  for clean-build matrices, full sanitizer matrices, and packaging verification.
- Preserve at least 30 GiB of free disk space. Do not place large generated
  artifacts in `/tmp` (a small tmpfs); use disk-backed build scratch such as
  `build/tmp` where supported. Clean only confirmed stale LinuxAPO artifacts,
  never files owned by active tests or unrelated applications.
- Prioritize the user's desktop and games. Do not kill, suspend, or change
  priority of user applications. If memory/CPU pressure is high, defer
  nonessential local work rather than risk responsiveness or OOM.
- Keep mandatory release gates; move them to CI or defer a release until they
  can be verified. Record actual test evidence and never report unrun tests as
  passing.
