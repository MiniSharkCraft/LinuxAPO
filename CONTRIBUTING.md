# Contributing to LinuxAPO

Thanks for your interest in improving LinuxAPO (SkyAPO)! Contributions, bug reports, documentation improvements, and reproducible test cases are welcome.

## Before you start

- Check existing [issues](https://github.com/MiniSharkCraft/LinuxAPO/issues) and [discussions](https://github.com/MiniSharkCraft/LinuxAPO/discussions) to avoid duplicate work.
- For substantial behavior changes, open an issue or discussion first to agree on scope.
- Review [README.md](README.md), [ROADMAP.md](ROADMAP.md), [AGENTS.md](AGENTS.md), and the relevant documents under [docs/](docs/).
- Follow the repository's existing license and third-party attribution requirements. The main license text is in [COPYING](COPYING).

## Development workflow

1. Fork the repository and create a topic branch.
2. Clone with submodules, or initialize submodules in an existing checkout: `git submodule update --init --recursive`.
3. Configure and build using the instructions and supported options documented in the README and CMake files.
4. Add or update focused tests for behavior changes.
5. Run the relevant CTest tests and static checks that your environment supports.
6. Open a pull request describing the change, test evidence, and any remaining limitations.

## Realtime audio requirements

Audio callbacks must remain safe for realtime execution. Avoid blocking I/O, uncontrolled allocation, waiting on locks, logging, and unbounded processing on realtime threads. Pay attention to object lifetime, graph replacement, PipeWire state transitions, plugin behavior, and reported latency/PDC. Prefer changes that are small, measurable, and covered by regression tests.

## Pull request checklist

- [ ] Scope and rationale are explained.
- [ ] Relevant tests were run, with commands and results recorded.
- [ ] Realtime and thread-safety implications were considered.
- [ ] Documentation was updated when user-visible behavior changed.
- [ ] No unrelated generated files, secrets, or binary artifacts are included.

Please be respectful when reviewing others' work. Maintainers may ask for changes before merging; a submitted PR does not guarantee acceptance.
