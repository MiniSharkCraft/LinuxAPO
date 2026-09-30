#!/usr/bin/env python3
"""Extract Equalizer APO's upstream realtime process methods into build tree.

The loader, configuration parser, registry access, and Windows notification
thread in FilterEngine.cpp are deliberately excluded. Only the marked realtime
method block is copied; the one Windows semaphore notification is translated
to a lock-free atomic handoff consumed by the non-realtime owner.
"""

from pathlib import Path
import argparse
import sys


BEGIN = "#pragma AVRT_CODE_BEGIN"
END = "#pragma AVRT_CODE_END"
UPSTREAM_RELEASE = "ReleaseSemaphore(loadSemaphore, 1, NULL);"
LINUX_HANDOFF = "transitionComplete.store(true, std::memory_order_release);"
UPSTREAM_COUNTER_RESET = "transitionCounter = 0;"
LINUX_COUNTER_RESET = (
    "completedTransitionCounter = transitionCounter;\n"
    "\t\ttransitionCounter = 0;"
)


def generate(source: Path, destination: Path) -> None:
    text = source.read_text(encoding="utf-8-sig")
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise ValueError("upstream FilterEngine.cpp realtime markers changed")
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    if body.count("FilterEngine::process(") != 2:
        raise ValueError("expected exactly two upstream process overloads")
    if body.count(UPSTREAM_RELEASE) != 2:
        raise ValueError("expected two transition semaphore releases")
    if body.count(UPSTREAM_COUNTER_RESET) != 2:
        raise ValueError("expected two transition counter resets")
    body = body.replace("FilterEngine::process(",
                        "UpstreamFilterEngineProcess::process(")
    body = body.replace(UPSTREAM_RELEASE, LINUX_HANDOFF)
    body = body.replace(UPSTREAM_COUNTER_RESET, LINUX_COUNTER_RESET)
    generated = (
        "/* Generated from official Equalizer APO FilterEngine.cpp.\n"
        " * Copyright (C) 2014 Jonas Thedering; GPL-2.0-or-later.\n"
        " * Do not edit: regenerate with generate_upstream_filter_engine_process.py.\n"
        " */\n"
        '#include "UpstreamFilterEngineProcess.h"\n'
        '#include "FilterConfiguration.h"\n'
        "#include <cstring>\n"
        + BEGIN + body + END + "\n"
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_text() != generated:
        destination.write_text(generated)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("upstream", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        generate(args.upstream, args.output)
    except (OSError, ValueError) as error:
        print(f"upstream FilterEngine adaptation failed: {error}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
