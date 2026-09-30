#!/usr/bin/env python3
"""CLI-level checks for diagnostics JSON and absent-daemon null handling."""

import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading


STATUS = (
    'Daemon: streaming\n'
    'Selected device: USB "Mic" \\ Desk\n'
    'Capture node: 43 (Capture "Port" \\ 1)\n'
    'Virtual microphone: SkyAPO Virtual Mic\n'
    'Virtual node: 45 (skyapo.virtual_mic)\n'
    'Format: F32 planar DSP\n'
    'Channels: 2\n'
    'Channel positions: FL FR\n'
    'Sample rate: 48000 Hz\n'
    'Quantum: unknown\n'
    'Filters: 1\n'
    'Plugin-reported latency sum: 64 samples (no delay compensation)\n'
    'Filter chain:\n'
    '  Preamp: -6 dB — /tmp/chain "quoted" \\ name.txt:1\n'
    'Config: /tmp/skyapo "quoted" \\ config.txt\n'
    'Processed blocks: 120\n'
    'Overruns: 0\n'
    'Active capture links: 2/2\n'
    'Process average: 12.5 us\n'
    'Process maximum: 23 us\n'
    'Input RMS: 0.7\n'
    'Output RMS: 0.35\n'
    'DSP amplitude ratio: 0.5\n'
    'Plugin failures:\n'
    '  Plugin: CLAP org.skyapo.test.error-once — /tmp/config.txt:3\n'
)


def query_with_fixture(
    executable: str, runtime: pathlib.Path, config: pathlib.Path, json_mode=True
):
    socket_path = runtime / "skyapo.sock"
    socket_path.unlink(missing_ok=True)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(socket_path))
    server.listen(1)

    def respond():
        connection, _ = server.accept()
        request = connection.recv(128)
        assert request == b"SKYAPO/1 STATUS\n"
        payload = STATUS.encode("utf-8")
        connection.sendall(
            b"SKYAPO/1 OK " + str(len(payload)).encode("ascii") + b"\n" + payload
        )
        connection.close()
        server.close()

    worker = threading.Thread(target=respond)
    worker.start()
    environment = os.environ.copy()
    environment.update(
        XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config), HOME=str(config)
    )
    command = [executable, "diagnostics"]
    if json_mode:
        command.append("--json")
    result = subprocess.run(
        command,
        env=environment,
        check=True,
        text=True,
        capture_output=True,
    )
    worker.join(timeout=2)
    if worker.is_alive():
        raise RuntimeError("status fixture server did not finish")
    return json.loads(result.stdout) if json_mode else result.stdout


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: diagnostics_json_test.py /path/to/skyapo")
    executable = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="skyapo-json-test-") as temporary:
        root = pathlib.Path(temporary)
        runtime = root / "runtime"
        config = root / "config"
        runtime.mkdir(mode=0o700)
        config.mkdir()
        skyapo_config = config / "skyapo"
        skyapo_config.mkdir()
        (skyapo_config / "device").write_text('stable "device" \\ id\n')

        document = query_with_fixture(executable, runtime, config)
        assert document["daemon_state"] == "streaming"
        assert document["selected_device"] == {
            "configured_identifier": 'stable "device" \\ id',
            "runtime_name": 'USB "Mic" \\ Desk',
        }
        assert document["capture_node"] == {
            "id": 43,
            "description": 'Capture "Port" \\ 1',
        }
        assert document["virtual_microphone"]["node"] == {
            "id": 45,
            "name": "skyapo.virtual_mic",
        }
        assert document["quantum_frames"] is None
        assert document["sample_rate_hz"] == 48000
        assert document["plugin_reported_latency_sum_samples"] == 64
        assert abs(document["plugin_reported_latency_sum_ms"] - 64 / 48) < 1e-9
        assert document["channel_positions"] == ["FL", "FR"]
        assert document["filter_chain"] == [
            'Preamp: -6 dB — /tmp/chain "quoted" \\ name.txt:1'
        ]
        assert document["config_path"] == '/tmp/skyapo "quoted" \\ config.txt'
        assert document["process_average_us"] == 12.5
        assert document["dsp_amplitude_ratio"] == 0.5
        assert document["plugin_failures"] == [
            "Plugin: CLAP org.skyapo.test.error-once — /tmp/config.txt:3"
        ]

        text_output = query_with_fixture(executable, runtime, config, False)
        assert text_output.startswith("SkyAPO version: ")
        assert "Daemon: streaming\n" in text_output
        assert 'Selected device: USB "Mic" \\ Desk\n' in text_output

        environment = os.environ.copy()
        environment.update(
            XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config), HOME=str(config)
        )
        unavailable = subprocess.run(
            [executable, "diagnostics", "--json"],
            env=environment,
            check=True,
            text=True,
            capture_output=True,
        )
        absent = json.loads(unavailable.stdout)
        assert absent["daemon_state"] == "not reachable"
        assert absent["capture_node"] is None
        assert absent["virtual_microphone"] is None
        assert absent["sample_rate_hz"] is None
        assert absent["plugin_reported_latency_sum_samples"] is None
        assert absent["plugin_reported_latency_sum_ms"] is None
        assert absent["filter_chain"] is None
        assert absent["plugin_failures"] is None
    print("diagnostics JSON escaping, fields, and unknown/null checks passed")


if __name__ == "__main__":
    main()
