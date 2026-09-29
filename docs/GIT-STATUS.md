# Git handoff

Root repository initialized; no root commit created. Official upstream remains clean and its full history resides in `.git/modules/upstream/equalizerapo`, referenced by a mode-160000 gitlink pinned to `bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687`. Build outputs/audio recordings are ignored. New root files have intent-to-add entries so ordinary `git diff --stat` shows their content.

Snapshot of `git status --short` immediately before adding this report:

```text
 A .gitignore
A  .gitmodules
 A CMakeLists.txt
 A README.md
 A cmake/PortableEapo.cmake
 A docs/PORTING.md
 A docs/REALTIME.md
 A examples/basic.txt
 A examples/iir.txt
 A examples/preamp.txt
 A src/cli/main.cpp
 A src/core/Engine.cpp
 A src/core/Engine.h
 A src/daemon/main.cpp
 A src/pipewire/DeviceManager.cpp
 A src/pipewire/DeviceManager.h
 A src/pipewire/Runtime.cpp
 A src/pipewire/Runtime.h
 A src/platform/PlatformChannels.h
 A src/platform/RealtimeAudit.cpp
 A src/platform/RealtimeAudit.h
 A src/platform/Settings.h
 A src/platform/linux/MemoryHelper.cpp
 A src/platform/linux/compat.cpp
 A src/platform/linux/stdafx.h
 A src/render/main.cpp
 A tests/core_tests.cpp
 A tests/pipewire_lifetime.cpp
 A tests/realtime_probe.cpp
 A tests/realtime_safety_tests.cpp
 A tests/render_tests.cpp
A  upstream/equalizerapo
```

`git diff --stat` at that snapshot: **30 files changed, 1873 insertions(+)**. This includes existing project files because no root baseline commit existed; it is not a claim that all those files were implemented in this milestone. Staged `.gitmodules` and the submodule are additional entries shown by `git diff --cached --stat`. Adding this report adds one more intent-to-add root file. `git -C upstream/equalizerapo status --short` was empty.

For current exact output:

```sh
git status --short
git diff --stat
git diff --cached --stat
git -C upstream/equalizerapo status --short
git ls-files --stage upstream/equalizerapo
```
