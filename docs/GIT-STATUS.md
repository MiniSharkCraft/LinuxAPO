# Git handoff

The root repository now has an ordinary commit history. Initial prototype baseline: `a7b187f feat(pipewire): establish realtime audio prototype`. The Include/hot-reload implementation is committed separately after its build, CTest, sanitizer and realtime verification; see `git log --oneline --decorate -20` for current state.

The official SourceForge EAPO repository is a submodule at `.gitmodules`, pinned at `bbfcc3e5024cbb9d61ba75fc88d78605cc4c9687`. Its Git metadata is stored in `.git/modules/upstream/equalizerapo`; only a mode-160000 gitlink is tracked. The upstream worktree must remain clean. Build folders and recorded microphone WAVs are ignored.

Run these commands for the authoritative current handoff state:

```sh
git status --short
git diff --stat
git diff --cached --stat
git log --oneline --decorate -20
git -C upstream/equalizerapo status --short
git ls-files --stage upstream/equalizerapo
```
