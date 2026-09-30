#!/usr/bin/env python3
"""Pin SkyAPO outcomes for configuration samples shipped by upstream EAPO."""

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
    executable = Path(sys.argv[1]).resolve()
    upstream = Path(sys.argv[2]).resolve()
    configs = upstream / "Setup" / "config"

    portable = {
        "iir_lowpass.txt": "Valid config: 1 filters",
        "selective_delay.txt": "Valid config: 6 filters",
    }
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

    print("upstream config compatibility: 2 portable pass, 4 external-format reject")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"upstream config compatibility failed: {error}", file=sys.stderr)
        raise SystemExit(1)
