#!/usr/bin/env python3
"""Validate staged SkyAPO package entrypoints without audio hardware."""

import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


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


def run_ui_smoke(executable, env):
    """Check that the packaged Qt UI starts its event loop without a display."""
    ui_env = env.copy()
    ui_env["QT_QPA_PLATFORM"] = "offscreen"
    process = subprocess.Popen(
        [str(executable)], env=ui_env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        time.sleep(1.0)
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise RuntimeError(
                "packaged skyapo-ui exited before its startup window elapsed "
                f"(status {process.returncode}):\nstdout:\n{stdout}\n"
                f"stderr:\n{stderr}"
            )
    finally:
        if process.poll() is None:
            # Stop the isolated process group as well, so a short-lived CLI
            # request made by the UI cannot be left behind by this smoke test.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
                raise RuntimeError("packaged skyapo-ui did not terminate on SIGTERM")
        process.communicate(timeout=5)


def validate_desktop_metadata(root):
    """Reject syntactically valid packages with stale launch metadata."""
    metainfo_path = root / "usr/share/metainfo/org.skyapo.SkyAPO.metainfo.xml"
    try:
        component = ET.parse(metainfo_path).getroot()
    except ET.ParseError as error:
        raise RuntimeError(f"invalid AppStream metainfo XML: {error}") from error
    if component.tag != "component" or component.get("type") != "desktop-application":
        raise RuntimeError("AppStream metainfo must describe a desktop-application component")
    if component.findtext("id") != "org.skyapo.skyapo":
        raise RuntimeError("AppStream component ID must use the canonical SkyAPO application ID")
    if component.findtext("name") != "SkyAPO":
        raise RuntimeError("AppStream component name is missing or incorrect")
    homepage = component.find("url[@type='homepage']")
    if homepage is None or homepage.text != "https://sourceforge.net/projects/equalizerapo/":
        raise RuntimeError("AppStream metadata must link to the official Equalizer APO project")
    launchable = component.find("launchable")
    if launchable is None or launchable.get("type") != "desktop-id" or launchable.text != "skyapo.desktop":
        raise RuntimeError("AppStream launchable must refer to skyapo.desktop")

    desktop_path = root / "usr/share/applications/skyapo.desktop"
    entries = {}
    in_desktop_group = False
    for line in desktop_path.read_text().splitlines():
        if line.startswith("[") and line.endswith("]"):
            in_desktop_group = line == "[Desktop Entry]"
        elif in_desktop_group and "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            entries[key] = value
    if entries.get("Type") != "Application":
        raise RuntimeError("desktop entry Type must be Application")
    if entries.get("Exec") != "skyapo-ui":
        raise RuntimeError("desktop entry Exec must name the installed skyapo-ui executable")
    if entries.get("Icon") != "skyapo":
        raise RuntimeError("desktop entry Icon must match the installed skyapo icon")

    provided_binaries = [node.text for node in component.findall("./provides/binary")]
    if not provided_binaries:
        raise RuntimeError("AppStream metainfo must list the package's provided binaries")
    for binary in provided_binaries:
        if not binary or not (root / "usr/bin" / binary).is_file():
            raise RuntimeError(f"AppStream lists a missing package binary: {binary!r}")


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("usage: package_smoke_test.py PKGDIR")
    root = pathlib.Path(sys.argv[1])
    required = [
        "usr/bin/skyapo",
        "usr/bin/skyapod",
        "usr/bin/skyapo-render",
        "usr/bin/skyapo-bench",
        "usr/bin/skyapo-ui",
        "usr/lib/systemd/user/skyapod.service",
        "usr/share/applications/skyapo.desktop",
        "usr/share/metainfo/org.skyapo.SkyAPO.metainfo.xml",
        "usr/share/icons/hicolor/scalable/apps/skyapo.svg",
        "usr/share/man/man1/skyapo.1",
        "usr/share/man/man1/skyapod.1",
        "usr/share/man/man1/skyapo-bench.1",
        "usr/share/licenses/skyapo/EqualizerAPO-License.txt",
        "usr/share/licenses/skyapo/Qt-LGPL-3.0-License.txt",
    ]
    for relative in required:
        path = root / relative
        if not path.is_file():
            raise RuntimeError(f"package is missing required file: {relative}")
    validate_desktop_metadata(root)
    qt_license = (root / "usr/share/licenses/skyapo/Qt-LGPL-3.0-License.txt").read_text(errors="replace")
    if "GNU LESSER GENERAL PUBLIC LICENSE" not in qt_license:
        raise RuntimeError("package Qt LGPL license text is invalid")
    for binary in (
        "skyapo", "skyapod", "skyapo-render", "skyapo-bench", "skyapo-ui"
    ):
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
        runtime_dir = temp / "runtime"
        runtime_dir.mkdir(mode=0o700)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)

        cli = root / "usr/bin/skyapo"
        run([str(cli), "--help"], env)
        run([str(root / "usr/bin/skyapo-bench"), "--help"], env)
        config = temp / "smoke.txt"
        config.write_text("Preamp: -6 dB\n")
        run([str(cli), "config", "check", str(config)], env)
        # Renderer parsing must start and reject an incomplete invocation; do
        # not open devices or spawn an external audio process in package tests.
        render = run([str(root / "usr/bin/skyapo-render")], env, expected=2)
        if "usage: skyapo-render" not in render.stderr:
            raise RuntimeError("offline renderer did not print its usage on missing arguments")
        run_ui_smoke(root / "usr/bin/skyapo-ui", env)

    print("Staged package paths, user unit, CLI/render entrypoints and offscreen UI startup verified.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"package smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
