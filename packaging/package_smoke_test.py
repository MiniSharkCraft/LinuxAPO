#!/usr/bin/env python3
"""Validate a staged SkyAPO package without starting its daemon or GUI."""

import os
import pathlib
import subprocess
import sys
import tempfile


def run(command, env, expected=0):
    result = subprocess.run(
        command, env=env, text=True, capture_output=True, timeout=15
    )
    if result.returncode != expected:
        raise RuntimeError(
            f"{command!r} returned {result.returncode}, expected {expected}:\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("usage: package_smoke_test.py PKGDIR")
    root = pathlib.Path(sys.argv[1])
    required = [
        "usr/bin/skyapo",
        "usr/bin/skyapod",
        "usr/bin/skyapo-render",
        "usr/bin/skyapo-ui",
        "usr/lib/systemd/user/skyapod.service",
        "usr/share/applications/skyapo.desktop",
        "usr/share/metainfo/org.skyapo.SkyAPO.metainfo.xml",
        "usr/share/icons/hicolor/scalable/apps/skyapo.svg",
        "usr/share/licenses/skyapo/EqualizerAPO-License.txt",
    ]
    for relative in required:
        path = root / relative
        if not path.is_file():
            raise RuntimeError(f"package is missing required file: {relative}")
    for binary in ("skyapo", "skyapod", "skyapo-render", "skyapo-ui"):
        path = root / "usr/bin" / binary
        if not os.access(path, os.X_OK):
            raise RuntimeError(f"package binary is not executable: {path}")

    unit = (root / "usr/lib/systemd/user/skyapod.service").read_text()
    if "ExecStart=/usr/bin/skyapod" not in unit:
        raise RuntimeError("installed user unit has an incorrect ExecStart")

    with tempfile.TemporaryDirectory(prefix="skyapo-package-smoke-") as temp:
        temp = pathlib.Path(temp)
        env = os.environ.copy()
        env.update(
            HOME=str(temp / "home"),
            XDG_CONFIG_HOME=str(temp / "config"),
            XDG_CACHE_HOME=str(temp / "cache"),
            XDG_DATA_HOME=str(temp / "data"),
            XDG_STATE_HOME=str(temp / "state"),
        )
        for key in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                    "XDG_DATA_HOME", "XDG_STATE_HOME"):
            pathlib.Path(env[key]).mkdir(parents=True, exist_ok=True)

        cli = root / "usr/bin/skyapo"
        run([str(cli), "--help"], env)
        config = temp / "smoke.txt"
        config.write_text("Preamp: -6 dB\n")
        run([str(cli), "config", "check", str(config)], env)
        # Renderer parsing must start and reject an incomplete invocation; do
        # not open devices or spawn an external audio process in package tests.
        render = run([str(root / "usr/bin/skyapo-render")], env, expected=2)
        if "usage: skyapo-render" not in render.stderr:
            raise RuntimeError("offline renderer did not print its usage on missing arguments")

    print("Staged package paths, user unit and safe CLI/render entrypoints verified.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"package smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
