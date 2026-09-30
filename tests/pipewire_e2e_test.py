#!/usr/bin/env python3
"""Launch an isolated PipeWire graph and verify captured DSP output."""

import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time


def run(args, env, timeout=8, check=True):
    result = subprocess.run(
        args, env=env, text=True, capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def stop(process, label):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)
        raise RuntimeError(f"{label} did not stop cleanly after SIGTERM")


def shutdown(processes):
    errors = []
    for process, label in processes:
        try:
            stop(process, label)
        except Exception as error:
            errors.append(str(error))
    if errors:
        raise RuntimeError("; ".join(errors))


def main():
    if len(sys.argv) != 8:
        raise RuntimeError(
            "usage: pipewire_e2e_test.py PIPEWIRE DAEMON CLI SOURCE CONSUMER "
            "PIPEWIRE_CONFIG DSP_CONFIG")
    pipewire, daemon, cli, source, consumer, pw_config, dsp_config = map(
        pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="skyapo-pipewire-e2e-") as temp:
        root = pathlib.Path(temp)
        runtime = root / "runtime"
        config_home = root / "config"
        runtime.mkdir(mode=0o700)
        config_home.mkdir(mode=0o700)
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
        })
        logs = (root / "pipewire.log").open("w+")
        source_log = (root / "source.log").open("w+")
        daemon_log = (root / "daemon.log").open("w+")
        server = source_process = daemon_process = None
        try:
            server = subprocess.Popen(
                [str(pipewire), "--config", str(pw_config)], env=env,
                stdout=logs, stderr=subprocess.STDOUT)
            socket = runtime / "pipewire-0"
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and not socket.exists():
                if server.poll() is not None:
                    raise RuntimeError("private PipeWire server exited during startup")
                time.sleep(0.05)
            if not socket.exists():
                raise RuntimeError("private PipeWire server socket was not created")

            source_process = subprocess.Popen(
                [str(source)], env=env, stdout=source_log,
                stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if source_process.poll() is not None:
                    raise RuntimeError("deterministic PipeWire source exited")
                devices = run([str(cli), "device", "list"], env, timeout=5)
                if "skyapo.test.input" in devices.stdout:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("deterministic capture source was not enumerated")

            selected = run([str(cli), "device", "set", "skyapo.test.input"], env)
            if "skyapo.test.input" not in selected.stdout:
                raise RuntimeError(f"device selection did not persist: {selected.stdout}")
            current = run([str(cli), "device", "current"], env)
            if "skyapo.test.input" not in current.stdout:
                raise RuntimeError(f"selected source mismatch: {current.stdout}")

            daemon_process = subprocess.Popen(
                [str(daemon), "--config", str(dsp_config)], env=env,
                stdout=daemon_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            status = ""
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    raise RuntimeError("skyapod exited during startup")
                result = run([str(cli), "status"], env, timeout=5, check=False)
                status = result.stdout
                if (result.returncode == 0 and "Daemon: streaming" in status and
                        "SkyAPO Virtual Mic" in status and
                        "Sample rate: 48000 Hz" in status):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"daemon did not reach streaming state:\n{status}")
            for expected in (
                "Channels: 2", "Filters: 1", "Active capture links: 2/2"):
                if expected not in status:
                    raise RuntimeError(f"missing runtime value {expected!r}:\n{status}")

            captured = run([str(consumer)], env, timeout=12)
            match = re.search(r"RMS ratio to expected: ([0-9.]+)",
                              captured.stdout)
            if not match or abs(float(match.group(1)) - 1.0) >= 0.03:
                raise RuntimeError(
                    "capture output did not report the expected -6 dB:\n"
                    f"{captured.stdout}")
            print("Private PipeWire end-to-end capture passed.")
            print(status.rstrip())
            print(captured.stdout.rstrip())
        except Exception:
            for label, log in (("PipeWire server", logs),
                               ("test source", source_log),
                               ("skyapod", daemon_log)):
                log.flush()
                log.seek(0)
                details = log.read()
                if details:
                    print(f"--- {label} log ---\n{details}", file=sys.stderr)
            raise
        finally:
            try:
                shutdown(((daemon_process, "skyapod"),
                          (source_process, "test source"),
                          (server, "PipeWire server")))
            finally:
                for log in (daemon_log, source_log, logs):
                    log.flush()
                    log.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # include subprocess diagnostics in CTest output
        print(f"PipeWire E2E failure: {error}", file=sys.stderr)
        sys.exit(1)
