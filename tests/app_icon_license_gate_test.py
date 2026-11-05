#!/usr/bin/env python3
"""Ensure unlicensed branded artwork cannot be enabled by accident."""

import os
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: app_icon_license_gate_test.py CMAKE SOURCE_ROOT")
    cmake, source_root = sys.argv[1:]
    source_root = pathlib.Path(source_root).resolve()
    audit = (source_root / "docs/UI_LICENSE_AUDIT.md").read_text(
        encoding="utf-8"
    )
    status = re.search(
        r"(?m)^Release artwork redistribution status: (CLEARED|EXCLUDED)$",
        audit,
    )
    if status is None:
        raise RuntimeError("artwork audit must state CLEARED or EXCLUDED")
    if status.group(1) == "CLEARED":
        print("Artwork rights are cleared; negative opt-in test is not applicable.")
        return

    temp_root = pathlib.Path(
        os.environ.get("SKYAPO_TEST_TEMP_DIRECTORY")
        or os.environ.get("TMPDIR")
        or tempfile.gettempdir()
    )
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="skyapo-icon-rights-gate-", dir=temp_root
    ) as temporary:
        result = subprocess.run(
            [
                cmake,
                "-S",
                str(source_root),
                "-B",
                str(pathlib.Path(temporary) / "build"),
                "-DSKYAPO_BUNDLE_BRANDED_APP_ICON=ON",
            ],
            text=True,
            capture_output=True,
            timeout=60,
            check=False,
        )
    output = result.stdout + result.stderr
    normalized_output = " ".join(output.split())
    if result.returncode == 0:
        raise RuntimeError("CMake accepted branded artwork while rights are EXCLUDED")
    if "redistribution rights are documented as CLEARED" not in normalized_output:
        raise RuntimeError(
            "CMake rejected the opt-in for an unexpected reason:\n" + output
        )
    print("CMake rejected branded artwork opt-in until redistribution rights clear.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"application icon license gate test failed: {error}", file=sys.stderr)
        sys.exit(1)
