#!/usr/bin/env python3
"""Exercise SkyAPO after a real Arch package install, then remove it.

The normal mode must run as root inside a disposable Arch container and takes
the package archive as its only argument. ``--root`` is a non-mutating payload
smoke mode for an extracted package tree; it deliberately skips pacman and
system-wide systemd checks.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


EXECUTABLES = (
    "skyapo",
    "skyapod",
    "skyapo-render",
    "skyapo-bench",
    "skyapo-ui",
)
FIXED_FILES = (
    "usr/lib/systemd/user/skyapod.service",
    "usr/share/applications/skyapo.desktop",
    "usr/share/metainfo/org.skyapo.SkyAPO.metainfo.xml",
    "usr/share/icons/hicolor/scalable/apps/skyapo.svg",
    "usr/share/licenses/skyapo/EqualizerAPO-License.txt",
    "usr/share/licenses/skyapo/Qt-LGPL-3.0-License.txt",
    "usr/share/licenses/skyapo/CLAP-License.txt",
    "usr/share/licenses/skyapo/Steinberg-VST3-base-License.txt",
    "usr/share/licenses/skyapo/Steinberg-VST3-pluginterfaces-License.txt",
    "usr/share/licenses/skyapo/Steinberg-VST3-public-sdk-License.txt",
    "usr/share/licenses/skyapo/MuParserX-License.txt",
)
MAN_PAGES = ("skyapo.1", "skyapod.1", "skyapo-bench.1")
PACKAGE_FILES = tuple(f"usr/bin/{binary}" for binary in EXECUTABLES) + FIXED_FILES


def run(command, env=None, expected=0):
    result = subprocess.run(
        command, env=env, text=True, capture_output=True, timeout=60
    )
    if result.returncode != expected:
        raise RuntimeError(
            f"{command!r} returned {result.returncode}, expected {expected}:\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def isolated_env(temp):
    temp = pathlib.Path(temp)
    env = os.environ.copy()
    for key in (
        "HOME",
        "XDG_CONFIG_HOME",
        "XDG_CACHE_HOME",
        "XDG_DATA_HOME",
        "XDG_STATE_HOME",
    ):
        value = temp / key.lower()
        value.mkdir(parents=True, exist_ok=True)
        env[key] = str(value)
    runtime = temp / "runtime"
    runtime.mkdir(mode=0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime)
    return env


def render_gain_test(root, env, render_test_binary):
    bin_dir = pathlib.Path(root) / "usr/bin"
    cli = bin_dir / "skyapo"
    renderer = bin_dir / "skyapo-render"
    config = pathlib.Path(__file__).resolve().parent.parent / "examples/preamp.txt"
    check = run([str(cli), "config", "check", str(config)], env)
    if "valid" not in check.stdout.lower():
        raise RuntimeError(f"installed CLI did not confirm valid config: {check.stdout}")
    result = run([str(render_test_binary), str(renderer), str(config)], env)
    if "WAV render sample gain and format test passed at -6" not in result.stdout:
        raise RuntimeError(f"installed offline render test did not confirm its gain result: {result.stdout}")
    print(f"Installed offline WAV render verified: {result.stdout.strip()}")


def validate_payload(root, env, verify_systemd, render_test_binary):
    root = pathlib.Path(root)
    for relative in PACKAGE_FILES:
        path = root / relative
        if not path.is_file():
            raise RuntimeError(f"installed package is missing {relative}")
    for binary in EXECUTABLES:
        path = root / "usr/bin" / binary
        if not os.access(path, os.X_OK):
            raise RuntimeError(f"installed binary is not executable: {path}")
    for page in MAN_PAGES:
        if not any((root / f"usr/share/man/man1/{page}{suffix}").is_file() for suffix in ("", ".gz")):
            raise RuntimeError(f"installed man page is missing: {page}")

    unit_path = root / "usr/lib/systemd/user/skyapod.service"
    unit = unit_path.read_text(encoding="utf-8")
    if "ExecStart=/usr/bin/skyapod" not in unit:
        raise RuntimeError("installed skyapod user service has an incorrect ExecStart")
    if verify_systemd:
        verifier = shutil.which("systemd-analyze")
        if not verifier:
            raise RuntimeError("systemd-analyze is required for installed-unit validation")
        run([verifier, "verify", str(unit_path)], env)

    version = run([str(root / "usr/bin/skyapo"), "--version"], env)
    if "SkyAPO" not in version.stdout or "Equalizer APO" not in version.stdout:
        raise RuntimeError(f"installed CLI returned an unexpected version: {version.stdout}")
    render_gain_test(root, env, render_test_binary)


def package_is_installed():
    return subprocess.run(
        ["pacman", "-Q", "skyapo"], text=True, capture_output=True
    ).returncode == 0


def assert_package_payload_removed():
    for relative in PACKAGE_FILES:
        path = pathlib.Path("/") / relative
        if path.exists():
            raise RuntimeError(f"pacman -Rns left package file behind: {path}")
    for page in MAN_PAGES:
        for suffix in ("", ".gz"):
            path = pathlib.Path("/usr/share/man/man1") / f"{page}{suffix}"
            if path.exists():
                raise RuntimeError(f"pacman -Rns left package man page behind: {path}")


def pacman_install_test(package, render_test_binary):
    if os.geteuid() != 0:
        raise RuntimeError("pacman install/remove test must run as root in a disposable container")
    if package_is_installed():
        raise RuntimeError("expected a fresh container, but skyapo is already installed")

    primary_error = None
    installed_files = []
    install_succeeded = False
    try:
        installation = run(["pacman", "-U", "--noconfirm", str(package)])
        install_succeeded = True
        print("Pacman install transaction:")
        print(installation.stdout.rstrip())
        if installation.stderr.strip():
            print(installation.stderr.rstrip(), file=sys.stderr)
        query = run(["pacman", "-Q", "skyapo"])
        if not query.stdout.startswith("skyapo "):
            raise RuntimeError(f"pacman reported an unexpected package: {query.stdout}")
        print(f"Pacman installed package: {query.stdout.strip()}")
        integrity = run(["pacman", "-Qk", "skyapo"])
        print(f"Installed package file integrity: {integrity.stdout.strip()}")
        files = run(["pacman", "-Qlq", "skyapo"])
        installed_files = [
            pathlib.Path(line)
            for line in files.stdout.splitlines()
            if pathlib.Path(line).is_file()
        ]
        with tempfile.TemporaryDirectory(prefix="skyapo-installed-test-") as temp:
            validate_payload(
                "/", isolated_env(temp), verify_systemd=True,
                render_test_binary=render_test_binary,
            )
    except Exception as error:
        primary_error = error
        raise
    finally:
        if package_is_installed():
            removal = subprocess.run(
                ["pacman", "-Rns", "--noconfirm", "skyapo"],
                text=True,
                capture_output=True,
                timeout=120,
            )
            if removal.returncode != 0:
                detail = f"pacman -Rns failed:\n{removal.stdout}\n{removal.stderr}"
                if primary_error is not None:
                    print(detail, file=sys.stderr)
                else:
                    raise RuntimeError(detail)
            else:
                print("Pacman removal transaction:")
                print(removal.stdout.rstrip())
                if removal.stderr.strip():
                    print(removal.stderr.rstrip(), file=sys.stderr)
        if package_is_installed():
            detail = "pacman -Rns left the skyapo package installed"
            if primary_error is not None:
                print(detail, file=sys.stderr)
            else:
                raise RuntimeError(detail)
        elif install_succeeded:
            print("pacman -Rns removed skyapo successfully.")
            assert_package_payload_removed()
            remaining = [str(path) for path in installed_files if path.exists()]
            if remaining:
                raise RuntimeError(
                    "pacman -Rns left package-owned files behind: "
                    + ", ".join(remaining)
                )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", nargs="?", help="Arch package archive to install")
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        help="validate an extracted/staged package tree without pacman mutations",
    )
    parser.add_argument(
        "--render-test-binary",
        type=pathlib.Path,
        required=True,
        help="built skyapo-render-tests harness that verifies the installed renderer",
    )
    args = parser.parse_args()

    if args.root:
        with tempfile.TemporaryDirectory(prefix="skyapo-payload-test-") as temp:
            validate_payload(
                args.root, isolated_env(temp), verify_systemd=False,
                render_test_binary=args.render_test_binary,
            )
        print("Extracted package payload smoke passed; pacman/systemd checks were skipped.")
        return
    if not args.package:
        parser.error("package archive is required unless --root is used")
    package = pathlib.Path(args.package).resolve()
    if not package.is_file():
        raise RuntimeError(f"package archive does not exist: {package}")
    pacman_install_test(package, args.render_test_binary)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"fresh install test failed: {error}", file=sys.stderr)
        sys.exit(1)
