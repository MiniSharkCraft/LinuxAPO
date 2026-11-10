#!/usr/bin/env python3
"""Optional realtime smoke for an installed third-party CLAP or LV2 plugin.

This deliberately is not registered with CTest: the plugins are user-installed
third-party dependencies, not project fixtures or release dependencies.
The test uses a private PipeWire server and never changes desktop audio routes.
"""

from __future__ import annotations

import argparse
import json
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

PLUGIN_DEFAULTS = {
    "clap": {
        "plugin_id": "in.lsp-plug.filter_stereo",
        "gain_parameter": "2683833795",
        "gain_scale": "db",
        "path_variable": "CLAP_PATH",
        "default_paths": ("/usr/lib/clap", "/usr/local/lib/clap", "/usr/lib64/clap"),
    },
    "lv2": {
        "plugin_id": "http://drobilla.net/plugins/mda/Loudness",
        "gain_parameter": "output",
        "gain_scale": "mda_loudness",
        "path_variable": "LV2_PATH",
        "default_paths": ("/usr/lib/lv2", "/usr/local/lib/lv2", "/usr/lib64/lv2"),
    },
}
SAMPLE_RATE = 48000
GAIN_DELTA_DB = 6.0
MAX_RMS_RATIO_ERROR = 0.05


def verify_live_graph(pw_dump: Path, env: dict[str, str], status: str) -> str:
    capture = re.search(r"^Capture node: (\d+)", status, re.MULTILINE)
    virtual = re.search(r"^Virtual node: (\d+)", status, re.MULTILINE)
    if not capture or not virtual:
        raise RuntimeError("daemon status lacks capture or virtual-mic node ID")

    capture_id, virtual_id = capture.group(1), virtual.group(1)
    deadline = time.monotonic() + 5.0
    last_nodes: dict[str, str] = {}
    last_links: set[tuple[str, str]] = set()
    while time.monotonic() < deadline:
        dump = subprocess.run(
            [str(pw_dump)], env=env, text=True, capture_output=True,
            timeout=5, check=False,
        )
        if dump.returncode:
            raise RuntimeError(f"pw-dump failed during recording: {dump.stderr}")
        objects = json.loads(dump.stdout)
        nodes: dict[str, dict[str, object]] = {}
        links: set[tuple[str, str]] = set()
        for item in objects:
            info = item.get("info", {})
            props = info.get("props", item.get("props", {}))
            if item.get("type", "").endswith(":Node"):
                nodes[str(item.get("id"))] = props
            elif item.get("type", "").endswith(":Link"):
                output_node = info.get("output-node-id", props.get("link.output.node"))
                input_node = info.get("input-node-id", props.get("link.input.node"))
                if output_node is not None and input_node is not None:
                    links.add((str(output_node), str(input_node)))

        named_nodes = {
            str(props.get("node.name")): node_id
            for node_id, props in nodes.items()
        }
        input_source = named_nodes.get("skyapo.test.input")
        consumer = named_nodes.get("skyapo.test.consumer")
        last_nodes, last_links = named_nodes, links
        if input_source == capture_id and consumer and (
            (capture_id, virtual_id) in links and
            (virtual_id, consumer) in links
        ):
            return (
                f"skyapo.test.input[{capture_id}] -> "
                f"SkyAPO Virtual Mic/DSP[{virtual_id}] -> "
                f"skyapo.test.consumer[{consumer}]"
            )
        time.sleep(0.05)

    raise RuntimeError(
        "PipeWire graph did not show selected source -> SkyAPO virtual mic "
        "processing node -> recording consumer links; "
        f"status capture={capture_id}, virtual={virtual_id}; "
        f"nodes={last_nodes}, links={sorted(last_links)}"
    )


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
        "--format", choices=tuple(PLUGIN_DEFAULTS), default="clap",
        help="native plugin format to smoke-test (default: clap)",
    )
    parser.add_argument("--plugin-id", help="override the format's default plugin ID")
    parser.add_argument(
        "--gain-parameter", help="override the format's input-gain parameter ID/symbol"
    )
    parser.add_argument(
        "--gain-scale", choices=("db", "linear", "mda_loudness"),
        help="parameter units (defaults to the format's known plugin units)",
    )
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
        "--lv2-path", default=os.environ.get("LV2_PATH"),
        help="LV2 bundle search path (defaults to LV2_PATH or common system paths)",
    )
    parser.add_argument(
        "--tmp-root", type=Path, default=PROJECT_ROOT / "build/tmp",
        help="disk-backed parent for private runtime/config/test artifacts",
    )
    parser.add_argument(
        "--runtime-root", type=Path,
        default=Path(os.environ.get("XDG_RUNTIME_DIR", tempfile.gettempdir())),
        help="short-lived socket directory parent (defaults to XDG_RUNTIME_DIR or /tmp)",
    )
    parser.add_argument(
        "--gain-db", type=parse_gain_delta, default=GAIN_DELTA_DB,
        help="non-zero plugin gain change in [-20, 20] dB (default: 6)",
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
    runtime_root = args.runtime_root.resolve()
    executables = {
        name: build_dir / name
        for name in (
            "skyapo", "skyapod", "skyapo-pipewire-test-source",
            "skyapo-pipewire-test-consumer",
        )
    }
    missing = [str(path) for path in executables.values() if not path.is_file()]
    pipewire = Path("/usr/bin/pipewire")
    pw_dump = Path("/usr/bin/pw-dump")
    if not pipewire.is_file():
        missing.append(str(pipewire))
    if not pw_dump.is_file():
        missing.append(str(pw_dump))
    if not args.pipewire_config.is_file():
        missing.append(str(args.pipewire_config))
    if missing:
        raise RuntimeError("required test files are missing: " + ", ".join(missing))

    temp_root.mkdir(parents=True, exist_ok=True)
    runtime_root.mkdir(parents=True, exist_ok=True)
    plugin_defaults = PLUGIN_DEFAULTS[args.format]
    plugin_id = args.plugin_id or plugin_defaults["plugin_id"]
    gain_parameter = args.gain_parameter or plugin_defaults["gain_parameter"]
    gain_scale = args.gain_scale or plugin_defaults["gain_scale"]
    if args.format == "clap":
        plugin_path = args.clap_path or ":".join(plugin_defaults["default_paths"])
    else:
        plugin_path = args.lv2_path or ":".join(plugin_defaults["default_paths"])
    expected_ratio = 10.0 ** (args.gain_db / 20.0)
    if gain_scale == "linear":
        start_parameter = "1"
        changed_parameter = f"{expected_ratio:.9g}"
    elif gain_scale == "mda_loudness":
        start_parameter = "0.5"
        direction = 1.0 if args.gain_db > 0.0 else -1.0
        level = 0.5 + direction * 0.5 * math.sqrt(abs(args.gain_db) / 60.0)
        changed_parameter = f"{level:.9g}"
    else:
        start_parameter = "0"
        changed_parameter = f"{args.gain_db:g}"
    children: list[subprocess.Popen[str]] = []
    logs = []

    with tempfile.TemporaryDirectory(
        prefix="third-party-plugin-", dir=temp_root
    ) as temp, tempfile.TemporaryDirectory(
        prefix="lapo-", dir=runtime_root
    ) as runtime_temp:
        root = Path(temp)
        runtime = Path(runtime_temp)
        config_home = root / "config"
        state_home = root / "state"
        for directory in (config_home, state_home):
            directory.mkdir(mode=0o700)

        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
            "TMPDIR": str(temp_root),
        })
        env[plugin_defaults["path_variable"]] = plugin_path
        if args.format == "clap":
            env["SKYAPO_CLAP_PATHS_ONLY"] = "1"

        try:
            plugin_info = subprocess.run(
                [str(executables["skyapo"]), "plugin", "info", plugin_id],
                env=env, text=True, capture_output=True, timeout=15,
                check=False,
            )
            if plugin_info.returncode:
                raise RuntimeError(
                    f"cannot load {plugin_id}; check the {args.format.upper()} search path:\n"
                    f"{plugin_info.stdout}{plugin_info.stderr}"
                )
            parameter_line = next(
                (
                    line for line in plugin_info.stdout.splitlines()
                    if line.split("\t", 1)[0] == gain_parameter
                ),
                None,
            )
            if parameter_line is None:
                raise RuntimeError(
                    f"{plugin_id} does not expose expected parameter "
                    f"{gain_parameter}:\n{plugin_info.stdout}"
                )
            parameter_fields = parameter_line.split("\t")
            parameter_name = parameter_fields[1] if len(parameter_fields) > 1 else gain_parameter

            pipewire_log = (root / "pipewire.log").open("w+")
            source_log = (root / "source.log").open("w+")
            daemon_log = (root / "daemon.log").open("w+")
            logs.extend((pipewire_log, source_log, daemon_log))
            private_config = make_rate_config(
                args.pipewire_config,
                root / "pipewire-48000.conf",
                SAMPLE_RATE,
            )
            try:
                server = start_private_pipewire(
                    pipewire, private_config, env, pipewire_log, runtime
                )
            except RuntimeError as error:
                pipewire_log.flush()
                pipewire_log.seek(0)
                raise RuntimeError(
                    f"{error}; PipeWire log:\n{pipewire_log.read()}"
                ) from error
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

            config = root / f"third-party-{args.format}.txt"
            config.write_text(
                f"Plugin: {args.format.upper()} {plugin_id} "
                f"{gain_parameter}={start_parameter}\n",
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
                f"daemon failed to activate the {plugin_id} graph",
            )

            def capture(label: str) -> tuple[int, float, str, str]:
                process = subprocess.Popen(
                    [
                        str(executables["skyapo-pipewire-test-consumer"]),
                        "--accept-any-audio", "--expected-rate", str(SAMPLE_RATE),
                    ],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                children.append(process)
                graph = verify_live_graph(pw_dump, env, run_cli("status").stdout)
                stdout, stderr = process.communicate(timeout=12)
                result = subprocess.CompletedProcess(
                    process.args, process.returncode, stdout, stderr
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
                return (
                    int(frames.group(1)), float(rms.group(1)),
                    result.stdout.strip(), graph,
                )

            frames0, rms0, output0, _graph0 = capture("baseline")
            config.write_text(
                f"Plugin: {args.format.upper()} {plugin_id} "
                f"{gain_parameter}={changed_parameter}\n",
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
            frames1, rms1, output1, graph1 = capture(f"+{args.gain_db:g} dB")
            if not rms0 or not rms1:
                raise RuntimeError("third-party plugin produced silent output")
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

            print(
                f"Plugin: third-party {args.format.upper()} {plugin_id}, "
                f"parameter {gain_parameter} ({parameter_name})"
            )
            print(
                "Graph: private PipeWire, F32 planar, 48 kHz stereo, "
                "test source -> SkyAPO Virtual Mic DSP -> consumer"
            )
            print(f"pw-dump during independent recording: {graph1}")
            print(f"0 dB: {frames0} frames, RMS {rms0:.8f}")
            print(f"+{args.gain_db:g} dB: {frames1} frames, RMS {rms1:.8f}")
            print(
                f"Measured ratio {measured_ratio:.6f}; expected "
                f"{expected_ratio:.6f}; deviation {deviation:.3%}"
            )
            print(
                "SkyAPO callback allocation/free=0/0; overruns=0; "
                "normal non-sanitized build; no sanitizer markers in daemon log"
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
        print(f"third-party plugin PipeWire smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
