#!/usr/bin/env python3
"""Smoke-test CLI help/version paths that must not require a daemon."""

import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_basics_test.py /path/to/skyapo")
    executable = sys.argv[1]

    help_result = subprocess.run(
        [executable, "--help"], check=False, text=True, capture_output=True
    )
    assert help_result.returncode == 0, help_result.stderr
    for command in (
        "status",
        "device list",
        "device set <id-or-name>",
        "config check --json <file>",
        "plugin info <URI>",
    ):
        assert command in help_result.stdout, command

    version_result = subprocess.run(
        [executable, "--version"], check=False, text=True, capture_output=True
    )
    assert version_result.returncode == 0, version_result.stderr
    assert "SkyAPO " in version_result.stdout
    assert "Equalizer APO upstream " in version_result.stdout
    assert len(version_result.stdout.splitlines()) == 2

    print("CLI help and version passed")


if __name__ == "__main__":
    main()
