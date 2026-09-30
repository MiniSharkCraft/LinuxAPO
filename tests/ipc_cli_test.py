#!/usr/bin/env python3
"""Exercise Unix-socket IPC framing through the public CLI without PipeWire."""

import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading


def cli_exchange(executable, command, request, response):
    with tempfile.TemporaryDirectory(prefix="skyapo-ipc-test-") as temporary:
        root = pathlib.Path(temporary)
        runtime = root / "runtime"
        config = root / "config"
        runtime.mkdir(mode=0o700)
        config.mkdir()
        socket_path = runtime / "skyapo.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(socket_path))
        server.listen(1)
        received = []

        def serve_once():
            connection, _ = server.accept()
            data = bytearray()
            while chunk := connection.recv(128):
                data.extend(chunk)
            received.append(bytes(data))
            connection.sendall(response)
            connection.close()
            server.close()

        worker = threading.Thread(target=serve_once)
        worker.start()
        environment = os.environ.copy()
        environment.update(
            XDG_RUNTIME_DIR=str(runtime),
            XDG_CONFIG_HOME=str(config),
            HOME=str(config),
        )
        result = subprocess.run(
            [executable, *command],
            env=environment,
            check=False,
            text=True,
            capture_output=True,
        )
        worker.join(timeout=2)
        if worker.is_alive():
            raise RuntimeError("IPC fixture server did not finish")
        assert received == [request], received
        return result


def ok(payload: bytes) -> bytes:
    return b"SKYAPO/1 OK " + str(len(payload)).encode("ascii") + b"\n" + payload


def err(payload: bytes) -> bytes:
    return b"SKYAPO/1 ERR " + str(len(payload)).encode("ascii") + b"\n" + payload


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: ipc_cli_test.py /path/to/skyapo")
    executable = os.path.abspath(sys.argv[1])

    status = b"Daemon: streaming\nSample rate: 48000 Hz\n"
    result = cli_exchange(
        executable, ["status"], b"SKYAPO/1 STATUS\n", ok(status)
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == status.decode()

    for response, expected_error in (
        (b"Daemon: legacy server\n", "unsupported legacy"),
        (b"SKYAPO/2 OK 0\n", "unsupported daemon IPC protocol version"),
        (b"SKYAPO/1 OK 10\nshort", "length mismatch"),
        (b"SKYAPO/1 MAYBE 0\n", "response status"),
        (
            err(b"unsupported command\n"),
            "unsupported command",
        ),
    ):
        result = cli_exchange(
            executable, ["status"], b"SKYAPO/1 STATUS\n", response
        )
        assert result.returncode != 0
        assert expected_error in result.stderr, result.stderr

    reply = b"Config reload succeeded\n"
    result = cli_exchange(
        executable,
        ["config", "reload"],
        b"SKYAPO/1 RELOAD\n",
        ok(reply),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == reply.decode()

    plugin_reply = b"Updated org.example/gain parameter Gain dB to 0.250000\n"
    plugin_request = (
        b"SKYAPO/1 PLUGIN_SET 16:org.example/gain 7:Gain dB 0.250000\n"
    )
    result = cli_exchange(
        executable,
        ["plugin", "set", "org.example/gain", "Gain dB", "0.25"],
        plugin_request,
        ok(plugin_reply),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == plugin_reply.decode()

    bypass_reply = b"Bypassed org.example/gain\n"
    bypass_request = b"SKYAPO/1 PLUGIN_BYPASS 16:org.example/gain 2:on\n"
    result = cli_exchange(
        executable,
        ["plugin", "bypass", "org.example/gain", "on"],
        bypass_request,
        ok(bypass_reply),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == bypass_reply.decode()

    print("IPC CLI framing, plugin parameter/bypass control, status compatibility, "
          "and peer rejection tests passed")


if __name__ == "__main__":
    main()
