#!/usr/bin/env python3
"""Optional realtime smoke for an installed LSP CLAP plugin.

This deliberately is not registered with CTest: the LSP plugin is a user
installed third-party dependency, not a project fixture or release dependency.
The test uses a private PipeWire server and never changes desktop audio routes.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))

from pipewire_e2e_test import (  # noqa: E402
    make_rate_config,
    start_private_pipewire,
    start_test_source,
)

PLUGIN_ID = "in.lsp-plug.filter_stereo"
INPUT_GAIN_ID = "2683833795"
SAMPLE_RATE = 48000
GAIN_DELTA_DB = 6.0
MAX_RMS_RATIO_ERROR = 0.05


def parse_gain_delta(value: str) -> float:
    try:
        gain_db = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("gain must be a finite number") from error
    if not math.isfinite(gain_db) or gain_db == 0.0 or abs(gain_db) > 20.0:
        raise argparse.ArgumentTypeError("gain delta must be non-zero and within [-20, 20] dB")
    return gain_db


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir", type=Path, default=PROJECT_ROOT / "build",
        help="directory containing skyapo, skyapod and PipeWire test helpers",
    )
    parser.add_argument(
        "--pipewire-config", type=Path,
        default=Path("/usr/share/pipewire/pipewire.conf"),
        help="base PipeWire config used for the private test server",
    )
    parser.add_argument(
        "--clap-path", default=os.environ.get("CLAP_PATH"),
        help="CLAP search path (defaults to CLAP_PATH or common system paths)",
    )
    parser.add_argument(
        "--tmp-root", type=Path, default=PROJECT_ROOT / "build/tmp",
        help="disk-backed parent for private runtime/config/test artifacts",
    )
    parser.add_argument(
        "--gain-db", type=parse_gain_delta, default=GAIN_DELTA_DB,
        help="non-zero Input gain change in [-20, 20] dB (default: 6)",
    )
    return parser.parse_args()


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    temp_root = args.tmp_root.resolve()
    executables = {
        name: build_dir / name
        for name in (
            "skyapo", "skyapod", "skyapo-pipewire-test-source",
            "skyapo-pipewire-test-consumer",
        )
    }
    missing = [str(path) for path in executables.values() if not path.is_file()]
    pipewire = Path("/usr/bin/pipewire")
    if not pipewire.is_file():
        missing.append(str(pipewire))
    if not args.pipewire_config.is_file():
        missing.append(str(args.pipewire_config))
    if missing:
        raise RuntimeError("required test files are missing: " + ", ".join(missing))

    temp_root.mkdir(parents=True, exist_ok=True)
    clap_path = args.clap_path or ":".join(
        ("/usr/lib/clap", "/usr/local/lib/clap", "/usr/lib64/clap")
    )
    expected_ratio = 10.0 ** (args.gain_db / 20.0)
    children: list[subprocess.Popen[str]] = []
    logs = []

    with tempfile.TemporaryDirectory(
        prefix="third-party-lsp-clap-", dir=temp_root
    ) as temp:
        root = Path(temp)
        runtime = root / "runtime"
        config_home = root / "config"
        state_home = root / "state"
        for directory in (runtime, config_home, state_home):
            directory.mkdir(mode=0o700)

        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
            "TMPDIR": str(temp_root),
            "CLAP_PATH": clap_path,
            "SKYAPO_CLAP_PATHS_ONLY": "1",
        })

        try:
            plugin_info = subprocess.run(
                [str(executables["skyapo"]), "plugin", "info", PLUGIN_ID],
                env=env, text=True, capture_output=True, timeout=15,
                check=False,
            )
            if plugin_info.returncode:
                raise RuntimeError(
                    f"cannot load {PLUGIN_ID}; check --clap-path/CLAP_PATH:\n"
                    f"{plugin_info.stdout}{plugin_info.stderr}"
                )
            if INPUT_GAIN_ID not in plugin_info.stdout:
                raise RuntimeError(
                    f"{PLUGIN_ID} does not expose expected Input gain parameter "
                    f"{INPUT_GAIN_ID}:\n{plugin_info.stdout}"
                )

            pipewire_log = (root / "pipewire.log").open("w+")
            source_log = (root / "source.log").open("w+")
            daemon_log = (root / "daemon.log").open("w+")
            logs.extend((pipewire_log, source_log, daemon_log))
            private_config = make_rate_config(
                args.pipewire_config,
                root / "pipewire-48000.conf",
                SAMPLE_RATE,
            )
            server = start_private_pipewire(
                pipewire, private_config, env, pipewire_log, runtime
            )
            children.append(server)
            source = start_test_source(
                executables["skyapo-pipewire-test-source"],
                False, env, source_log,
            )
            children.append(source)

            def run_cli(*command: str, timeout: int = 8) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(executables["skyapo"]), *command],
                    env=env, text=True, capture_output=True,
                    timeout=timeout, check=False,
                )

            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                listed = run_cli("device", "list", timeout=5)
                if listed.returncode == 0 and "skyapo.test.input" in listed.stdout:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(
                    "private deterministic source did not appear:\n"
                    f"{listed.stdout}{listed.stderr}"
                )
            selected = run_cli("device", "set", "skyapo.test.input")
            if selected.returncode:
                raise RuntimeError(
                    "could not select private test source:\n"
                    f"{selected.stdout}{selected.stderr}"
                )

            config = root / "third-party-clap.txt"
            config.write_text(
                f"Plugin: CLAP {PLUGIN_ID} {INPUT_GAIN_ID}=0\n",
                encoding="utf-8",
            )
            daemon = subprocess.Popen(
                [str(executables["skyapod"]), "--config", str(config)],
                env=env, stdout=daemon_log, stderr=subprocess.STDOUT,
                text=True,
            )
            children.append(daemon)

            def wait_status(predicate, description: str, timeout: float = 20) -> str:
                end = time.monotonic() + timeout
                last_status = ""
                while time.monotonic() < end:
                    result = run_cli("status")
                    last_status = result.stdout
                    if result.returncode == 0 and predicate(last_status):
                        return last_status
                    if daemon.poll() is not None:
                        break
                    time.sleep(0.05)
                daemon_log.flush()
                daemon_log.seek(0)
                raise RuntimeError(
                    f"{description}; status:\n{last_status}\n"
                    f"daemon log:\n{daemon_log.read()}"
                )

            wait_status(
                lambda status: (
                    "Daemon: streaming" in status
                    and "SkyAPO Virtual Mic" in status
                    and "Filters: 1" in status
                ),
                f"daemon failed to activate the {PLUGIN_ID} graph",
            )

            def capture(label: str) -> tuple[int, float, str]:
                result = subprocess.run(
                    [
                        str(executables["skyapo-pipewire-test-consumer"]),
                        "--accept-any-audio", "--expected-rate", str(SAMPLE_RATE),
                        "--duration-ms", "2500", "--capture-node",
                        "skyapo.virtual_mic",
                    ],
                    env=env, text=True, capture_output=True, timeout=12,
                    check=False,
                )
                if result.returncode:
                    raise RuntimeError(
                        f"{label} independent capture failed:\n"
                        f"{result.stdout}{result.stderr}"
                    )
                frames = re.search(r"Captured frames: (\d+)", result.stdout)
                rms = re.search(r"RMS: ([0-9.eE+-]+)", result.stdout)
                if not frames or not rms:
                    raise RuntimeError(
                        f"{label} capture lacks frame/RMS measurements:\n"
                        f"{result.stdout}{result.stderr}"
                    )
                return int(frames.group(1)), float(rms.group(1)), result.stdout.strip()

            frames0, rms0, output0 = capture("0 dB")
            config.write_text(
                f"Plugin: CLAP {PLUGIN_ID} {INPUT_GAIN_ID}={args.gain_db:g}\n",
                encoding="utf-8",
            )
            reload_result = run_cli("config", "reload", timeout=10)
            if reload_result.returncode:
                raise RuntimeError(
                    "config reload rejected the gain update:\n"
                    f"{reload_result.stdout}{reload_result.stderr}"
                )
            wait_status(
                lambda status: "Graph transition: stable" in status,
                "plugin graph transition did not finish after reload",
            )
            frames1, rms1, output1 = capture(f"+{args.gain_db:g} dB")
            if not rms0 or not rms1:
                raise RuntimeError("third-party CLAP produced silent output")
            measured_ratio = rms1 / rms0
            deviation = abs(measured_ratio / expected_ratio - 1.0)
            if deviation > MAX_RMS_RATIO_ERROR:
                raise RuntimeError(
                    f"measured gain ratio {measured_ratio:.6f} differs from "
                    f"expected {expected_ratio:.6f} by {deviation:.2%}\n"
                    f"0 dB capture:\n{output0}\n+{args.gain_db:g} dB capture:\n{output1}"
                )

            final_status = run_cli("status").stdout
            for invariant in (
                "Callback allocations: 0",
                "Callback deallocations: 0",
                "Overruns: 0",
            ):
                if invariant not in final_status:
                    raise RuntimeError(
                        f"runtime audit did not report {invariant!r}:\n"
                        f"{final_status}"
                    )
            daemon_log.flush()
            daemon_log.seek(0)
            log_text = daemon_log.read()
            sanitizer_lines = [
                line for line in log_text.splitlines()
                if "ERROR: AddressSanitizer" in line
                or "runtime error:" in line
                or "UndefinedBehaviorSanitizer" in line
            ]
            if sanitizer_lines:
                raise RuntimeError(
                    "sanitizer diagnostics from daemon:\n"
                    + "\n".join(sanitizer_lines)
                )

            print(f"Plugin: LSP CLAP {PLUGIN_ID}, parameter {INPUT_GAIN_ID} (Input gain)")
            print(
                "Graph: private PipeWire, F32 planar, 48 kHz stereo, "
                "skyapo.test.input -> SkyAPO Virtual Mic"
            )
            print(f"0 dB: {frames0} frames, RMS {rms0:.8f}")
            print(f"+{args.gain_db:g} dB: {frames1} frames, RMS {rms1:.8f}")
            print(
                f"Measured ratio {measured_ratio:.6f}; expected "
                f"{expected_ratio:.6f}; deviation {deviation:.3%}"
            )
            print(
                "SkyAPO callback allocation/free=0/0; overruns=0; "
                "no ASan/UBSan diagnostics in daemon log"
            )
            return 0
        finally:
            for process in reversed(children):
                stop_process(process)
            for log in logs:
                log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"third-party LSP CLAP PipeWire smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
