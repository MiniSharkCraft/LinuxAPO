#!/usr/bin/env python3
"""Pin SkyAPO outcomes for configuration samples shipped by upstream EAPO."""

import argparse
import subprocess
import sys
from pathlib import Path


def run(executable: Path, config: Path) -> tuple[int, str]:
    result = subprocess.run(
        [str(executable), "config", "check", str(config)],
        text=True,
        capture_output=True,
        check=False,
        timeout=30,
    )
    return result.returncode, result.stdout + result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument(
        "--expressions-enabled",
        action="store_true",
        help="MuParserX-backed EAPO Eval/inline expression support is built",
    )
    args = parser.parse_args()
    executable = args.executable.resolve()
    upstream = args.upstream.resolve()
    configs = upstream / "Setup" / "config"

    portable = {"selective_delay.txt": "Valid config: 6 filters"}
    if args.expressions_enabled:
        portable["iir_lowpass.txt"] = "Valid config: 1 filters"
    else:
        code, output = run(executable, configs / "iir_lowpass.txt")
        if code == 0 or "Eval/inline expressions require MuParserX support" not in output:
            raise AssertionError(
                "expression-dependent upstream fixture was not rejected with "
                f"the documented optional-feature diagnostic: exit={code}, "
                f"output={output!r}"
            )
    for name, expected in portable.items():
        code, output = run(executable, configs / name)
        if code != 0 or expected not in output:
            raise AssertionError(
                f"upstream portable fixture {name} failed: "
                f"exit={code}, output={output!r}"
            )

    # These are Room EQ Wizard export examples, not Equalizer APO command
    # files. Pin explicit rejection rather than silently claiming support.
    external_exports = {
        "example.txt": "example.txt:1: expected command:",
        "demo.txt": "demo.txt:1: expected command:",
        "config.txt": "example.txt:1: expected command:",
        "multichannel.txt": "demo.txt:1: expected command:",
    }
    for name, expected in external_exports.items():
        code, output = run(executable, configs / name)
        if code == 0 or expected not in output:
            raise AssertionError(
                f"upstream non-EAPO export {name} was not rejected with its "
                f"known source location: exit={code}, output={output!r}"
            )

    portable_count = len(portable)
    unavailable_count = 0 if args.expressions_enabled else 1
    print(
        "upstream config compatibility: "
        f"{portable_count} portable pass, "
        f"{unavailable_count} optional-expression reject, "
        "4 external-format reject"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"upstream config compatibility failed: {error}", file=sys.stderr)
        raise SystemExit(1)
