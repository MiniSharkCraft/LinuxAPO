# Plugin support status

No plugin host, scanner, plugin IPC, or plugin CLI exists yet. LV2, CLAP, Linux VST2, Linux VST3, and yabridge wrapper support remain roadmap work. SkyAPO does not embed Wine or modify yabridge. Do not add plugin directives to an active config: unsupported commands fail validation.

Before implementing hosts, audit format ABI and licenses; in particular, do not redistribute proprietary Steinberg SDK files without permission. Keep discovery/loading/state/GUI operations outside the audio callback. The target uses a format-neutral node interface and ordinary Linux hosts should discover yabridge-created wrappers through their exposed native format.
