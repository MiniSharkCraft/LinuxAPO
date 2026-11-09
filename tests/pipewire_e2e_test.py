#!/usr/bin/env python3
"""Launch an isolated PipeWire graph and verify captured DSP output."""

import json
import os
import pathlib
import re
import select
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from functools import lru_cache
from pprint import pformat

from pipewire_dump import pipewire_dump_objects


def run(args, env, timeout=8, check=True):
    result = subprocess.run(
        args, env=env, text=True, capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def metadata_value(raw, key):
    """Return one PipeWire metadata value from pw-metadata output."""
    for line in raw.splitlines():
        if f"key:'{key}' " in line:
            match = re.search(
                rf"key:'{re.escape(key)}'\s+value:'(.*?)'(?:\s+type:'|$)",
                line)
            return match.group(1) if match else None
    return None


def metadata_node_name(raw, key):
    """Return a PipeWire node name encoded in a JSON metadata value."""
    value = metadata_value(raw, key)
    if value is None:
        return None
    try:
        node = json.loads(value)
    except json.JSONDecodeError:
        return None
    return node.get("name") if isinstance(node, dict) else None


@lru_cache(maxsize=None)
def wireplumber_supports_profiles(executable):
    """Return whether this WirePlumber uses the 0.5 profile/logging API."""
    executable = str(executable)
    help_result = subprocess.run(
        [executable, "--help"], text=True, capture_output=True, timeout=5)
    help_text = help_result.stdout + help_result.stderr
    return "--profile" in help_text


def wireplumber_command(executable):
    """Select the policy-only profile when this WirePlumber supports it."""
    if wireplumber_supports_profiles(str(executable)):
        return [executable, "--profile", "policy"]
    # WirePlumber 0.4 has no profile CLI option; its default configuration
    # starts the session and policy manager together.
    return [executable]


def start_wireplumber(executable, env, log):
    """Give only WirePlumber a private session bus for its policy service."""
    command = wireplumber_command(executable)
    dbus_run_session = shutil.which("dbus-run-session")
    if not dbus_run_session:
        raise RuntimeError(
            "WirePlumber E2E requires dbus-run-session; install dbus-daemon")
    return subprocess.Popen(
        [dbus_run_session, "--", *command], env=env, text=True,
        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)


def wait_for_wireplumber_client(pw_dump, env, process, timeout=8):
    """Wait until the private PipeWire registry exposes WirePlumber."""
    deadline = time.monotonic() + timeout
    last_clients = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                "private WirePlumber exited before registering with PipeWire")
        result = run([str(pw_dump)], env, timeout=5, check=False)
        if result.returncode == 0:
            objects = pipewire_dump_objects(result.stdout)
            last_clients = [
                item.get("info", {}).get("props", item.get("props", {}))
                for item in objects
                if item.get("type", "").endswith(":Client")]
            if any("wireplumber" in " ".join((
                    str(props.get("application.name", "")),
                    str(props.get("application.process.binary", ""))
            )).lower() for props in last_clients):
                return
        time.sleep(0.05)
    raise RuntimeError(
        "private PipeWire registry did not expose a WirePlumber client before "
        f"daemon startup; clients={last_clients}")


class SystemdManagedDaemon:
    """Popen-like control for a transient per-user skyapod.service."""

    def __init__(self, unit, systemctl):
        self.unit = unit
        self.systemctl = systemctl
        self.returncode = None

    def _show(self, properties):
        result = subprocess.run(
            [self.systemctl, "--user", "show", self.unit,
             *(f"--property={item}" for item in properties)],
            env=os.environ.copy(), text=True, capture_output=True, timeout=5)
        if result.returncode:
            return None
        return dict(line.split("=", 1) for line in result.stdout.splitlines()
                    if "=" in line)

    def main_pid(self):
        values = self._show(("MainPID",))
        return int(values.get("MainPID", "0")) if values else 0

    def restart_count(self):
        values = self._show(("NRestarts",))
        return int(values.get("NRestarts", "0")) if values else 0

    def poll(self):
        values = self._show(("ActiveState", "ExecMainStatus"))
        if not values:
            return None
        if values.get("ActiveState") in (
                "active", "activating", "reloading", "deactivating"):
            return None
        self.returncode = int(values.get("ExecMainStatus", "1"))
        return self.returncode

    def send_signal(self, sig):
        if sig == signal.SIGTERM:
            result = subprocess.run(
                [self.systemctl, "--user", "stop", self.unit],
                env=os.environ.copy(), text=True, capture_output=True,
                timeout=25)
        else:
            result = subprocess.run(
                [self.systemctl, "--user", "kill", f"--signal={sig}",
                 "--kill-whom=main", self.unit],
                env=os.environ.copy(), text=True, capture_output=True,
                timeout=5)
        if result.returncode:
            raise RuntimeError(
                f"systemctl could not signal {self.unit}: {result.stderr}")

    def kill(self):
        self.send_signal(signal.SIGKILL)

    def wait(self, timeout=None):
        deadline = time.monotonic() + (timeout if timeout is not None else 30)
        while time.monotonic() < deadline:
            result = self.poll()
            if result is not None:
                return result
            time.sleep(0.05)
        raise subprocess.TimeoutExpired(self.unit, timeout)

    def journal(self):
        result = subprocess.run(
            ["journalctl", "--user", "-u", self.unit, "-n", "40",
             "--no-pager", "-o", "cat"], env=os.environ.copy(),
            text=True, capture_output=True, timeout=5)
        return result.stdout or result.stderr


def create_null_sink(manager, properties):
    manager.stdin.write(f"create-node adapter {properties}\n")
    manager.stdin.flush()


def start_systemd_daemon(systemd_run, systemctl, daemon, config, env,
                         unit_template, restart_sec_override=None):
    template = pathlib.Path(unit_template).read_text()
    properties = {}
    for line in template.splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.strip().split("=", 1)
            properties[key] = value
    if properties.get("Restart") != "on-failure":
        raise RuntimeError(
            "installed skyapod user unit must set Restart=on-failure")
    restart_sec = properties.get("RestartSec")
    if not restart_sec:
        raise RuntimeError("skyapod user unit must define RestartSec")
    unit = f"skyapo-e2e-{os.getpid()}-{time.monotonic_ns()}"
    command = [
        str(systemd_run), "--user", f"--unit={unit}", "--no-block",
        "--property=Type=exec",
        f"--property=Restart={properties['Restart']}",
        f"--property=RestartSec={restart_sec_override or restart_sec}",
        "--property=StartLimitIntervalSec=0",
    ]
    for key in ("XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                "PIPEWIRE_RUNTIME_DIR"):
        command.append(f"--setenv={key}={env[key]}")
    command += [str(daemon), "--config", str(config)]
    result = subprocess.run(command, env=os.environ.copy(), text=True,
                            capture_output=True, timeout=10)
    if result.returncode:
        raise RuntimeError(
            f"systemd-run failed to launch isolated skyapod unit:\n"
            f"{result.stdout}\n{result.stderr}")
    return SystemdManagedDaemon(unit, str(systemctl))


def stop(process, label):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)
        raise RuntimeError(
            f"{label} did not stop cleanly within 20 seconds after SIGTERM")


def wait_for_daemon_log(daemon_process, daemon_log, start_offset, marker,
                        label, timeout=8):
    """Wait for a daemon log marker without racing its shared file offset."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if daemon_process.poll() is not None:
            raise RuntimeError(
                f"skyapod exited while waiting for {label} "
                f"(exit status {daemon_process.returncode})")
        size = os.fstat(daemon_log.fileno()).st_size
        if size > start_offset:
            length = min(size - start_offset, 65536)
            output = os.pread(daemon_log.fileno(), length, start_offset)
            if marker.encode() in output:
                return
        time.sleep(0.05)
    raise RuntimeError(f"skyapod did not report {label} within {timeout} seconds")


def wait_for_consumer_ready(process, label, timeout=8,
                            marker="SKYAPO_CONSUMER_READY"):
    if process.stdout is None:
        raise RuntimeError(f"{label} has no readiness output pipe")
    deadline = time.monotonic() + timeout
    captured = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        readable, _, _ = select.select(
            [process.stdout], [], [], min(0.05, deadline - time.monotonic()))
        if not readable:
            continue
        line = process.stdout.readline()
        if not line:
            break
        captured.append(line)
        if line.strip() == marker:
            return "".join(captured)
    raise RuntimeError(
        f"{label} did not capture a PipeWire buffer before the graph "
        f"transition (exit={process.poll()}):\n{''.join(captured)}")


def shutdown(processes):
    errors = []
    for process, label in processes:
        try:
            stop(process, label)
        except Exception as error:
            errors.append(str(error))
    if errors:
        raise RuntimeError("; ".join(errors))


def destroy_capture_link(pw_cli, pw_dump, env, status):
    capture = re.search(r"^Capture node: (\d+)", status, re.MULTILINE)
    virtual = re.search(r"^Virtual node: (\d+)", status, re.MULTILINE)
    if not capture or not virtual:
        raise RuntimeError("daemon status did not expose PipeWire node IDs")
    objects = pipewire_dump_objects(run([str(pw_dump)], env, timeout=5).stdout)
    source_id, sink_id = capture.group(1), virtual.group(1)
    candidates = []
    for item in objects:
        if not item.get("type", "").startswith("PipeWire:Interface:Link"):
            continue
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if (str(props.get("link.output.node", "")) == source_id and
                str(props.get("link.input.node", "")) == sink_id):
            candidates.append(item.get("id"))
    if not candidates:
        links = []
        for item in objects:
            if not item.get("type", "").startswith("PipeWire:Interface:Link"):
                continue
            info = item.get("info", {})
            props = info.get("props", item.get("props", {}))
            links.append({
                "id": item.get("id"),
                "output": props.get("link.output.node"),
                "output_port": props.get("link.output.port"),
                "input": props.get("link.input.node"),
                "input_port": props.get("link.input.port"),
            })
        raise RuntimeError(
            "could not find the physical-capture → SkyAPO link in pw-dump; "
            f"capture={source_id}, virtual={sink_id}, links={links}")
    link_id = str(candidates[0])
    run([str(pw_cli), "destroy", link_id], env, timeout=5)
    return link_id


def destroy_playback_internal_link(pw_cli, pw_dump, env):
    objects = pipewire_dump_objects(run([str(pw_dump)], env, timeout=5).stdout)
    node_ids = {}
    for item in objects:
        if not item.get("type", "").endswith(":Node"):
            continue
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if props.get("node.name") in (
                "skyapo.playback", "skyapo.playback.dsp",
                "skyapo.playback.stream"):
            node_ids[props["node.name"]] = str(item.get("id"))
    frontend_id = node_ids.get("skyapo.playback")
    playback_id = node_ids.get("skyapo.playback.dsp")
    stream_id = node_ids.get("skyapo.playback.stream")
    if not frontend_id or not playback_id or not stream_id:
        raise RuntimeError(
            "pw-dump lacks SkyAPO playback DSP/stream nodes: "
            f"{node_ids}")
    candidates = []
    frontend_links = []
    for item in objects:
        if not item.get("type", "").startswith("PipeWire:Interface:Link"):
            continue
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if (str(props.get("link.output.node", "")) == frontend_id and
                str(props.get("link.input.node", "")) == playback_id):
            frontend_links.append(item.get("id"))
        if (str(props.get("link.output.node", "")) == playback_id and
                str(props.get("link.input.node", "")) == stream_id):
            candidates.append(item.get("id"))
    if not frontend_links:
        raise RuntimeError(
            "could not find the public adapter -> SkyAPO DSP link; "
            f"frontend={frontend_id}, DSP={playback_id}")
    if not candidates:
        raise RuntimeError(
            "could not find the SkyAPO DSP -> playback stream link; "
            f"playback={playback_id}, stream={stream_id}")
    link_id = str(candidates[0])
    run([str(pw_cli), "destroy", link_id], env, timeout=5)
    return link_id


def virtual_source_id(status):
    match = re.search(r"^Virtual node: (\d+)", status, re.MULTILINE)
    if not match:
        raise RuntimeError("daemon status did not expose the virtual node ID")
    return match.group(1)


def assert_virtual_source(pw_dump, env, expected_id):
    objects = pipewire_dump_objects(run([str(pw_dump)], env, timeout=5).stdout)
    for item in objects:
        if str(item.get("id")) != str(expected_id):
            continue
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if (props.get("node.name") == "skyapo.virtual_mic" and
                props.get("node.description") == "SkyAPO Virtual Mic" and
                props.get("media.class") == "Audio/Source"):
            return
        raise RuntimeError(
            f"PipeWire node {expected_id} is not SkyAPO Virtual Mic: {props}")
    raise RuntimeError(f"PipeWire node {expected_id} disappeared from pw-dump")


def connect_test_source_to_playback(pw_cli, pw_dump, env, source_name,
                                   playback_name, channels):
    objects = pipewire_dump_objects(run([str(pw_dump)], env, timeout=5).stdout)
    nodes, ports = {}, {}
    for item in objects:
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        kind = item.get("type", "")
        if kind.endswith(":Node"):
            name = props.get("node.name")
            if name in (source_name, playback_name):
                nodes[name] = str(item.get("id"))
        elif kind.endswith(":Port"):
            node_id = str(props.get("node.id", ""))
            direction = props.get("port.direction")
            channel = props.get("audio.channel")
            if direction and channel:
                ports[(node_id, direction, channel)] = str(item.get("id"))
    if source_name not in nodes or playback_name not in nodes:
        raise RuntimeError(f"PipeWire test nodes missing: {nodes}")
    names = ("MONO",) if channels == 1 else ("FL", "FR")
    expected = set()
    for channel in names:
        output = ports.get((nodes[source_name], "out", channel))
        input_port = ports.get((nodes[playback_name], "in", channel))
        if not output or not input_port:
            raise RuntimeError(f"PipeWire {channel} test ports missing: {ports}")
        expected.add((nodes[source_name], nodes[playback_name], output,
                      input_port))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        graph = pipewire_dump_objects(run([str(pw_dump)], env, timeout=5).stdout)
        active = set()
        for item in graph:
            if not item.get("type", "").endswith(":Link"):
                continue
            info = item.get("info", {})
            props = info.get("props", item.get("props", {}))
            active.add((str(props.get("link.output.node", "")),
                        str(props.get("link.input.node", "")),
                        str(props.get("link.output.port", "")),
                        str(props.get("link.input.port", ""))))
        if expected <= active:
            if env.get("SKYAPO_TEST_TRACE_PIPEWIRE_GRAPH") == "1":
                snapshot = run([str(pw_dump)], env, timeout=5).stdout
                print("PipeWire graph at established playback routing:\n" +
                      summarize_graph_dump(snapshot))
            return
        time.sleep(0.05)
    raise RuntimeError(
        f"PipeWire did not establish source -> playback links; "
        f"expected={expected}, active={active}")


def wait_for_playback_application_route(process, pw_dump, env, channels,
                                        timeout=8):
    """Wait until the test application's channels actively enter SkyAPO."""
    expected_channels = (
        ("MONO",) if channels == 1 else
        ("FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR")[:channels])
    deadline = time.monotonic() + timeout
    graph = []
    active_channels = set()
    active_channel_link_counts = {}
    bypass_links = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                "WirePlumber playback stream exited before reaching "
                "SkyAPO Playback")
        graph = pipewire_dump_objects(
            run([str(pw_dump)], env, timeout=5).stdout)
        node_ids = {}
        port_channels = {}
        for item in graph:
            props = item.get("info", {}).get(
                "props", item.get("props", {}))
            kind = item.get("type", "")
            if kind.endswith(":Node"):
                name = props.get("node.name")
                if name in ("skyapo.test.playback-app", "skyapo.playback"):
                    node_ids[name] = str(item.get("id"))
            elif kind.endswith(":Port"):
                port_channels[str(item.get("id"))] = props.get(
                    "audio.channel")
        active_channels = set()
        active_channel_link_counts = {}
        bypass_links = []
        app_id = node_ids.get("skyapo.test.playback-app")
        playback_id = node_ids.get("skyapo.playback")
        if app_id and playback_id:
            for item in graph:
                if not item.get("type", "").endswith(":Link"):
                    continue
                info = item.get("info", {})
                if (str(info.get("output-node-id", "")) != app_id or
                        info.get("state") != "active"):
                    continue
                input_node = str(info.get("input-node-id", ""))
                if input_node != playback_id:
                    bypass_links.append(input_node)
                    continue
                channel = port_channels.get(
                    str(info.get("output-port-id", "")))
                if channel:
                    active_channels.add(channel)
                    active_channel_link_counts[channel] = (
                        active_channel_link_counts.get(channel, 0) + 1)
            if (set(expected_channels) <= active_channels and
                    all(active_channel_link_counts.get(channel) == 1
                        for channel in expected_channels) and
                    not bypass_links):
                print(
                    "Verified active PipeWire playback route: "
                    f"skyapo.test.playback-app ({app_id}) -> "
                    f"skyapo.playback ({playback_id}), channels "
                    f"{', '.join(channel for channel in expected_channels if channel in active_channels)}.")
                return
        time.sleep(0.05)
    raise RuntimeError(
        "ordinary playback did not establish active links to every SkyAPO "
        f"Playback channel; expected={expected_channels}, "
        f"active={sorted(active_channels)}, "
        f"links_per_channel={active_channel_link_counts}, "
        f"active_bypass_links={bypass_links}, "
        f"graph={summarize_graph_dump(json.dumps(graph))}")


def summarize_graph_dump(raw):
    objects = pipewire_dump_objects(raw)
    nodes = set()
    summary = []
    for item in objects:
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if item.get("type", "").endswith(":Node"):
            name = props.get("node.name", "")
            if (name.startswith("skyapo.") or
                    name.startswith("skyapo.test.")):
                nodes.add(str(item.get("id")))
                summary.append(item)
    for item in objects:
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        kind = item.get("type", "")
        if kind.endswith(":Port") and str(props.get("node.id", "")) in nodes:
            summary.append(item)
        elif kind.endswith(":Link"):
            endpoints = {str(info.get("output-node-id", "")),
                         str(info.get("input-node-id", ""))}
            if endpoints & nodes:
                summary.append(item)
    return pformat(summary, width=100)


def trace_graph_if_requested(label, pw_dump, env):
    if env.get("SKYAPO_TEST_TRACE_PIPEWIRE_GRAPH") != "1":
        return
    result = run([str(pw_dump)], env, timeout=5, check=False)
    summary = (summarize_graph_dump(result.stdout)
               if result.returncode == 0 else
               f"pw-dump failed ({result.returncode}): {result.stderr}")
    print(f"PipeWire graph at {label}:\n{summary}")


def start_private_pipewire(pipewire, config, env, log, runtime):
    process = subprocess.Popen(
        [str(pipewire), "--config", str(config)], env=env,
        stdout=log, stderr=subprocess.STDOUT)
    socket = runtime / "pipewire-0"
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("private PipeWire server exited during startup")
        if socket.exists():
            return process
        time.sleep(0.05)
    raise RuntimeError("private PipeWire server socket was not created")


def make_rate_config(base_config, destination, sample_rate):
    text = base_config.read_text()
    replacements = {
        "default.clock.rate": str(sample_rate),
        "default.clock.allowed-rates": f"[ {sample_rate} ]",
        "default.clock.quantum": "1024",
    }
    for key, value in replacements.items():
        pattern = re.compile(rf"(?m)^(\s*)#\s*{re.escape(key)}\s*=.*$")
        text, count = pattern.subn(rf"\g<1>{key} = {value}", text)
        if count != 1:
            raise RuntimeError(
                f"expected exactly one commented {key} in {base_config}, "
                f"found {count}; refusing an unforced-rate test")
    destination.write_text(text)
    return destination


def start_test_source(source, mono, env, source_log, name=None, serial=None,
                      surround51=False, surround71=False,
                      latency_probe=False, link_node=None,
                      playback_stream=False, start_silent=False,
                      pdc_trigger_staging=False):
    args = [str(source)]
    if mono:
        args.append("--mono")
    elif surround51:
        args.append("--surround51")
    elif surround71:
        args.append("--surround71")
    if name is not None:
        args += ["--name", name]
    if serial is not None:
        args += ["--serial", serial]
    if latency_probe:
        args.append("--latency-probe")
    if link_node is not None:
        args += ["--link-node", link_node]
    if playback_stream:
        args.append("--playback-stream")
    if start_silent:
        args.append("--start-silent")
    if pdc_trigger_staging:
        args.append("--pdc-trigger-staging")
    return subprocess.Popen(
        args, env=env,
        stdout=source_log, stderr=subprocess.STDOUT)


def main():
    if len(sys.argv) not in (11, 13, 14) or sys.argv[10] not in (
        "mono", "stereo", "surround51", "surround71", "mono-44100", "stereo-44100",
        "mono-48000", "stereo-48000", "mono-96000", "stereo-96000",
        "latency", "convolution", "include-reload", "source-replug",
        "source-replug-stable-id", "callback-exception",
        "callback-transition-exception",
        "server-restart", "plugin-live-param", "clap-state-restart",
        "lv2-state-restart", "vst3-state-restart", "clap-process-failure",
        "vst3-process-failure", "vst3-exception-failure",
        "vst3-latency-nonstd-failure",
        "lv2-live-param",
        "vst3-live-param", "vst2-live-param", "plugin-bypass",
        "vst3-bypass", "lv2-bypass",
        "vst3-latency-change", "vst2-latency-change", "clap-latency-change",
        "lv2-latency-change", "lv2-pdc-final", "lv2-pdc-recovery",
        "lv2-pdc-config-commit-race", "lv2-pdc-format-event-order",
        "renegotiate",
        "transition-format", "device-filter", "stage-warning",
        "systemd-crash-restart",
        "playback-wireplumber-systemd-crash-restart",
        "playback-wireplumber-systemd-crash-user-override",
        "playback-wireplumber-recovery-write-failure",
        "playback-wireplumber-route-confirmation-race",
        "playback-wireplumber-route-sync-failure",
        "playback-wireplumber-route-confirmation-timeout",
        "playback-wireplumber-configured-write-failure",
        "device-switch", "delay-pdc", "source-consumer-latency",
        "playback-preamp", "playback-wireplumber",
        "playback-wireplumber-link-error",
        "playback-wireplumber-volume", "playback-wireplumber-mute",
        "playback-wireplumber-stage",
        "playback-wireplumber-target-recovery",
        "playback-wireplumber-target-recovery-stable-id",
        "playback-wireplumber-startup-recovery",
        "playback-wireplumber-server-restart",
        "playback-wireplumber-output-switch",
        "playback-wireplumber-surround51",
        "playback-wireplumber-surround71",
        "playback-wireplumber-lv2-latency",
        "playback-wireplumber-pdc-recovery",
        "playback-wireplumber-quantum-variation"
    ):
        raise RuntimeError(
            "usage: pipewire_e2e_test.py PIPEWIRE PW_CLI PW_DUMP DAEMON CLI "
            "SOURCE CONSUMER PIPEWIRE_CONFIG DSP_CONFIG "
            "mono|stereo|surround51|surround71|mono-44100|stereo-44100|mono-48000|stereo-48000|"
        "mono-96000|stereo-96000|latency|convolution|include-reload|source-replug|"
            "source-replug-stable-id|callback-exception|"
            "callback-transition-exception|"
            "plugin-live-param|clap-state-restart|lv2-state-restart|"
            "vst3-state-restart|clap-process-failure|vst3-process-failure|"
            "vst3-exception-failure|vst3-latency-nonstd-failure|"
            "lv2-live-param|vst3-live-param|"
            "vst2-live-param|plugin-bypass|vst3-bypass|lv2-bypass|vst3-latency-change|"
            "vst2-latency-change|"
            "clap-latency-change|lv2-latency-change|lv2-pdc-final|"
            "lv2-pdc-recovery|lv2-pdc-config-commit-race|"
            "renegotiate|transition-format|systemd-crash-restart|"
            "playback-wireplumber-systemd-crash-restart|"
            "playback-wireplumber-systemd-crash-user-override|"
            "playback-wireplumber-recovery-write-failure|"
            "playback-wireplumber-route-confirmation-race|"
            "playback-wireplumber-route-sync-failure|"
            "playback-wireplumber-route-confirmation-timeout|"
            "device-filter|stage-warning|device-switch|delay-pdc|"
            "source-consumer-latency|playback-preamp|playback-wireplumber|"
            "playback-wireplumber-link-error|"
            "playback-wireplumber-configured-write-failure|"
            "playback-wireplumber-volume|playback-wireplumber-mute|"
            "playback-wireplumber-stage|"
            "playback-wireplumber-target-recovery|"
            "playback-wireplumber-target-recovery-stable-id|"
            "playback-wireplumber-startup-recovery|"
            "playback-wireplumber-server-restart|"
            "playback-wireplumber-output-switch|"
            "playback-wireplumber-surround51|"
            "playback-wireplumber-surround71|"
            "playback-wireplumber-lv2-latency|"
            "playback-wireplumber-pdc-recovery|"
            "playback-wireplumber-quantum-variation")
    (pipewire, pw_cli, pw_dump, daemon, cli, source, consumer, pw_config,
     dsp_config) = map(pathlib.Path, sys.argv[1:10])
    mode = sys.argv[10]
    playback_link_async_error = mode == "playback-wireplumber-link-error"
    if playback_link_async_error:
        mode = "playback-wireplumber"
    playback_route_write_failure = (
        mode == "playback-wireplumber-configured-write-failure")
    stable_playback_target = mode == "playback-wireplumber-target-recovery-stable-id"
    startup_playback_recovery = mode == "playback-wireplumber-startup-recovery"
    playback_isolation = mode == "playback-wireplumber"
    recovery_playback_target = mode in (
        "playback-wireplumber-target-recovery",
        "playback-wireplumber-target-recovery-stable-id")
    output_switch = mode == "playback-wireplumber-output-switch"
    replacement_target_name = (
        "skyapo.test.null-target-renamed" if stable_playback_target else
        "skyapo.test.null-target")
    wireplumber = pathlib.Path(sys.argv[11]) if len(sys.argv) >= 13 else None
    playback_client = pathlib.Path(sys.argv[12]) if len(sys.argv) >= 13 else None
    ui_test = pathlib.Path(sys.argv[13]) if len(sys.argv) == 14 else None
    wireplumber_playback = mode.startswith("playback-wireplumber")
    playback_stage = mode == "playback-wireplumber-stage"
    playback_pdc_recovery = mode == "playback-wireplumber-pdc-recovery"
    playback_quantum_variation = mode == "playback-wireplumber-quantum-variation"
    playback_pdc_refresh = mode == "playback-wireplumber-lv2-latency"
    if wireplumber_playback and (wireplumber is None or playback_client is None):
        raise RuntimeError(
            "WirePlumber playback test needs WirePlumber and playback-client paths")
    surround51 = mode in ("surround51", "playback-wireplumber-surround51")
    surround71 = mode in ("surround71", "playback-wireplumber-surround71")
    rate_case = re.fullmatch(r"(mono|stereo)-(44100|48000|96000)", mode)
    mono = mode == "mono" or (rate_case is not None and
                               rate_case.group(1) == "mono")
    sample_rate = (int(rate_case.group(2)) if rate_case else
                   96000 if mode == "lv2-pdc-config-commit-race" else 48000)
    latency_plugin = mode == "latency"
    clap_process_failure = mode == "clap-process-failure"
    vst3_process_failure = mode == "vst3-process-failure"
    vst3_exception_failure = mode == "vst3-exception-failure"
    vst3_nonstd_latency_failure = mode == "vst3-latency-nonstd-failure"
    plugin_process_failure = (clap_process_failure or vst3_process_failure or
                              vst3_exception_failure or
                              vst3_nonstd_latency_failure)
    failing_plugin_id = (
        "org.skyapo.test.error-once" if clap_process_failure else
        "534B5941504F00010000000000000005" if vst3_exception_failure else
        "534B5941504F00010000000000000006" if vst3_nonstd_latency_failure else
        "534B5941504F00010000000000000002")
    include_reload = mode == "include-reload"
    stable_id_replug = mode == "source-replug-stable-id"
    source_replug = mode == "source-replug" or stable_id_replug
    playback_server_restart = mode == "playback-wireplumber-server-restart"
    server_restart = mode == "server-restart" or playback_server_restart
    playback_systemd_user_override = (
        mode == "playback-wireplumber-systemd-crash-user-override")
    playback_recovery_write_failure = (
        mode == "playback-wireplumber-recovery-write-failure")
    playback_route_confirmation_race = (
        mode == "playback-wireplumber-route-confirmation-race")
    playback_route_sync_failure = (
        mode == "playback-wireplumber-route-sync-failure")
    playback_route_confirmation_timeout = (
        mode == "playback-wireplumber-route-confirmation-timeout")
    playback_route_cleanup_failure = (
        playback_route_sync_failure or playback_route_confirmation_timeout)
    playback_systemd_crash_restart = mode in (
        "playback-wireplumber-systemd-crash-restart",
        "playback-wireplumber-systemd-crash-user-override")
    vst2_live = mode == "vst2-live-param"
    vst2_latency_change = mode == "vst2-latency-change"
    vst3_latency_change = mode == "vst3-latency-change"
    clap_latency_change = mode == "clap-latency-change"
    lv2_latency_change = mode in (
        "lv2-latency-change", "playback-wireplumber-lv2-latency")
    lv2_pdc_final = mode == "lv2-pdc-final"
    lv2_pdc_recovery = mode in (
        "lv2-pdc-recovery", "playback-wireplumber-pdc-recovery",
        "lv2-pdc-format-event-order")
    pdc_format_event_order = mode == "lv2-pdc-format-event-order"
    lv2_pdc_config_commit_race = mode == "lv2-pdc-config-commit-race"
    delay_pdc = mode == "delay-pdc"
    plugin_live = mode in (
        "plugin-live-param", "lv2-live-param", "vst3-live-param",
        "clap-state-restart", "lv2-state-restart", "vst3-state-restart") or vst2_live
    plugin_bypass = mode in ("plugin-bypass", "vst3-bypass", "lv2-bypass")
    renegotiate = mode == "renegotiate"
    transition_format = mode == "transition-format"
    device_filter = mode == "device-filter"
    stage_warning = mode == "stage-warning"
    device_switch = mode == "device-switch"
    systemd_crash_restart = (mode == "systemd-crash-restart" or
                             playback_systemd_crash_restart or
                             playback_systemd_user_override)
    latency_probe = mode == "source-consumer-latency"
    convolution = mode == "convolution"
    callback_process_exception = mode in (
        "callback-exception", "callback-transition-exception")
    callback_transition_exception = mode == "callback-transition-exception"
    plugin_chain = (plugin_live or plugin_bypass or latency_plugin or
                    plugin_process_failure or
                    vst3_latency_change or vst2_latency_change or
                    clap_latency_change or vst3_nonstd_latency_failure)
    plugin_chain = plugin_chain or lv2_latency_change
    plugin_chain = plugin_chain or lv2_pdc_final
    plugin_chain = plugin_chain or lv2_pdc_recovery or lv2_pdc_config_commit_race
    if mode in ("lv2-live-param", "lv2-state-restart"):
        live_plugin_id, live_parameter = (
            "https://skyapo.example/plugins/test-gain", "gain")
    elif mode in ("vst3-live-param", "vst3-state-restart"):
        live_plugin_id, live_parameter = (
            "534B5941504F00010000000000000001", "7")
    elif mode == "vst3-bypass":
        live_plugin_id = "534B5941504F00010000000000000001"
        live_parameter = "7"
    elif mode == "lv2-bypass":
        live_plugin_id = "https://skyapo.example/plugins/test-gain"
        live_parameter = "gain"
    elif vst2_live:
        live_plugin_id = os.environ["SKYAPO_TEST_VST2_PATH"]
        live_parameter = "Gain"
    else:
        live_plugin_id, live_parameter = "org.skyapo.test.gain", "Gain"
    device_name = (
        "skyapo.test.mono" if mono else
        "skyapo.test.surround51" if surround51 else
        "skyapo.test.surround71" if surround71 else "skyapo.test.input")
    channel_count = 1 if mono else 6 if surround51 else 8 if surround71 else 2
    channel_positions = (
        "MONO" if mono else
        "FL FR FC LFE RL RR" if surround51 else
        "FL FR FC LFE RL RR SL SR" if surround71 else "FL FR")
    with tempfile.TemporaryDirectory(prefix="skyapo-pipewire-e2e-") as temp:
        root = pathlib.Path(temp)
        runtime = root / "runtime"
        config_home = root / "config"
        state_home = root / "state"
        runtime.mkdir(mode=0o700)
        config_home.mkdir(mode=0o700)
        state_home.mkdir(mode=0o700)
        rate_config = make_rate_config(
            pw_config, root / f"pipewire-{sample_rate}.conf", sample_rate)
        if (renegotiate or transition_format or lv2_pdc_recovery or
                playback_stage):
            text = rate_config.read_text()
            text = text.replace(
                "default.clock.allowed-rates = [ 48000 ]",
                "default.clock.allowed-rates = [ 44100 48000 96000 ]")
            if "default.clock.allowed-rates = [ 44100 48000 96000 ]" not in text:
                raise RuntimeError("could not enable runtime test sample rates")
            rate_config.write_text(text)
        included_config = config_home / "include-reload-root.txt"
        transition_config = config_home / "transition-format.txt"
        pdc_recovery_config = config_home / "pdc-recovery.txt"
        convolution_config = config_home / "convolution.txt"
        plugin_failure_config = config_home / "plugin-process-failure.txt"
        callback_exception_config = config_home / "callback-exception.txt"
        include_directory = config_home / "includes"
        include_child = include_directory / "include-reload-child.txt"
        if include_reload:
            include_directory.mkdir()
            included_config.write_text(
                "Include: includes/include-reload-child.txt\n")
            include_child.write_text("Preamp: -6 dB\n")
            dsp_config = included_config
        if output_switch:
            cli_config = config_home / "skyapo" / "config.txt"
            cli_config.parent.mkdir()
            cli_config.write_text("Preamp: -6 dB\n")
            dsp_config = cli_config
        if transition_format:
            transition_config.write_text("Preamp: -6 dB\n")
            dsp_config = transition_config
        if convolution:
            impulse_path = config_home / "single-tap-half-gain.wav"
            with wave.open(str(impulse_path), "wb") as impulse:
                impulse.setnchannels(1)
                impulse.setsampwidth(2)
                impulse.setframerate(sample_rate)
                # PCM16 16384 represents exactly 0.5 in libsndfile's decoder.
                impulse.writeframes(struct.pack("<h", 16384))
            convolution_config.write_text(
                f'Convolution: "{impulse_path}"\n')
            dsp_config = convolution_config
        if lv2_pdc_recovery:
            dynamic_pdc_body = (
                "Channel: L\n"
                "Plugin: LV2 https://skyapo.example/plugins/test-latency-increase\n"
                "Channel: L R\n"
                "Copy: L=L" + "+R" * 430 + "\n")
            if playback_pdc_recovery:
                rejected_dynamic_pdc = (
                    'If: stage == "post-mix"\n' + dynamic_pdc_body +
                    "Else:\nPreamp: -6 dB\nEndIf:\n")
                # Start on a known-safe graph, then introduce the plugin while
                # an ordinary playback stream and independent monitor are
                # already active. This forces the real crossfade/runtime path.
                pdc_recovery_config.write_text(
                    'If: stage == "post-mix"\n'
                    "Preamp: -120 dB\n"
                    "Else:\nPreamp: -6 dB\nEndIf:\n")
            else:
                rejected_dynamic_pdc = dynamic_pdc_body
                pdc_recovery_config.write_text(rejected_dynamic_pdc)
            dsp_config = pdc_recovery_config
        if lv2_pdc_config_commit_race:
            pdc_recovery_config.write_text(
                "Channel: L\n"
                "Plugin: LV2 https://skyapo.example/plugins/"
                "test-latency-increase\n"
                "Channel: L R\n"
                "Copy: L=L" + "+R" * 430 + "\n")
            dsp_config = pdc_recovery_config
        if clap_process_failure:
            plugin_failure_config.write_text(
                "Preamp: -6 dB\n"
                "Plugin: CLAP org.skyapo.test.error-once\n")
            dsp_config = plugin_failure_config
        elif vst3_process_failure:
            plugin_failure_config.write_text(
                "Preamp: -6 dB\n"
                "Plugin: VST3 534B5941504F00010000000000000002\n")
            dsp_config = plugin_failure_config
        elif vst3_exception_failure:
            plugin_failure_config.write_text(
                "Preamp: -6 dB\n"
                "Plugin: VST3 534B5941504F00010000000000000005\n")
            dsp_config = plugin_failure_config
        elif vst3_nonstd_latency_failure:
            plugin_failure_config.write_text(
                "Preamp: -6 dB\n"
                "Plugin: VST3 534B5941504F00010000000000000006\n")
            dsp_config = plugin_failure_config
        if device_filter:
            device_config = root / "device-filter.txt"
            device_config.write_text(
                f"Device: unmatched microphone; {device_name}\n"
                'If: deviceName == "SkyAPO Deterministic Test Input"\n'
                'If: connectionName == "skyapo.test.input"\n'
                'If: stage == "capture"\n'
                "Preamp: -6 dB\n"
                "EndIf:\nEndIf:\nEndIf:\n")
            dsp_config = device_config
        if stage_warning:
            staged_config = root / "stage-warning.txt"
            staged_config.write_text(
                "Stage: pre-mix\nPreamp: 48 dB\n"
                "Stage: capture\nPreamp: -6 dB\n")
            dsp_config = staged_config
        if playback_stage:
            stage_config = root / "playback-stage.txt"
            stage_config.write_text(
                'If: stage == "post-mix"\n'
                "Preamp: -6 dB\n"
                "Else:\n"
                "Preamp: 0 dB\n"
                "EndIf:\n")
            dsp_config = stage_config
        elif startup_playback_recovery:
            stage_config = root / "playback-startup-stage.txt"
            stage_config.write_text(
                "Stage: capture\nPreamp: -6 dB\n"
                "Stage: post-mix\nPreamp: -6 dB\n")
            dsp_config = stage_config
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
            # Prevent GIO from trying to create a per-test GVFS FUSE mount
            # under this disposable runtime directory.
            "GIO_USE_VFS": "local",
        })
        if playback_recovery_write_failure:
            env["SKYAPO_TEST_FAIL_CONFIGURED_DEFAULT_WRITE_FILE"] = str(
                root / "fail-configured-default-write-once")
            env["SKYAPO_TEST_HOLD_DEFAULT_ROUTE_RECOVERY_FILE"] = str(
                root / "hold-default-route-recovery")
        if playback_route_confirmation_race:
            env["SKYAPO_TEST_REVERT_ACTIVE_BEFORE_ROUTE_SYNC"] = "1"
        if playback_route_confirmation_timeout:
            env["SKYAPO_TEST_SUPPRESS_DEFAULT_ROUTE_CONFIRMATION"] = "1"
        if output_switch or playback_isolation:
            fault_library = env.get("SKYAPO_TEST_FILTER_FAULT_LIBRARY")
            if not fault_library or not pathlib.Path(fault_library).is_file():
                raise RuntimeError(
                    "playback recovery test requires the test-only PipeWire "
                    "fault interposer")
            preload = [fault_library]
            if env.get("ASAN_OPTIONS"):
                sanitizer_runtime = subprocess.run(
                    ["ldd", str(daemon)], text=True, capture_output=True,
                    check=False)
                match = re.search(
                    r"libasan\.so(?:\.\d+)*\s+=>\s+(\S+)",
                    sanitizer_runtime.stdout)
                if sanitizer_runtime.returncode or not match:
                    raise RuntimeError(
                        "could not locate the daemon's ASan runtime before "
                        "preloading the test-only PipeWire fault interposer")
                preload.insert(0, match.group(1))
            if env.get("LD_PRELOAD"):
                preload.append(env["LD_PRELOAD"])
            env["LD_PRELOAD"] = ":".join(preload)
            if output_switch:
                fault_trigger = root / "fail-next-playback-stream-filter"
                fault_receipt = root / "playback-stream-filter-failure-triggered"
                env["SKYAPO_TEST_FILTER_FAULT_TRIGGER"] = str(fault_trigger)
                env["SKYAPO_TEST_FILTER_FAULT_RECEIPT"] = str(fault_receipt)
            if playback_isolation:
                link_fault_trigger = root / "fail-next-playback-internal-link"
                link_fault_receipt = root / (
                    "playback-internal-link-failure-triggered")
                env["SKYAPO_TEST_LINK_FAULT_TRIGGER"] = str(link_fault_trigger)
                env["SKYAPO_TEST_LINK_FAULT_RECEIPT"] = str(link_fault_receipt)
                if playback_link_async_error:
                    env["SKYAPO_TEST_LINK_FAULT_MODE"] = "async-error"
        if wireplumber_playback:
            env["WIREPLUMBER_DEBUG"] = os.environ.get(
                "SKYAPO_WIREPLUMBER_DEBUG",
                ("s-linking:D,s-node:D"
                 if wireplumber_supports_profiles(wireplumber)
                 else "I:script/policy-node"))
        if transition_format or playback_pdc_recovery:
            env["SKYAPO_TEST_TRANSITION_MS"] = "500"
        config_candidate_pause = root / "config-candidate-pause"
        if lv2_pdc_config_commit_race or playback_pdc_recovery:
            env["SKYAPO_TEST_PAUSE_AFTER_CONFIG_CANDIDATE"] = str(
                config_candidate_pause)
        if pdc_format_event_order:
            env["SKYAPO_TEST_FORMAT_REBUILD_BEFORE_PDC_EVENT"] = "1"
        if mode == "lv2-pdc-recovery":
            # Exercise the control-plane timeout path independently from the
            # plugin's PDC budget failure. The daemon must remain reachable,
            # faulted, and silent when callback draining cannot be confirmed.
            env["SKYAPO_TEST_FAIL_LATENCY_DRAIN"] = "1"
        if callback_process_exception:
            env["SKYAPO_TEST_THROW_PROCESS_EXCEPTION_ONCE"] = (
                "after-transition" if callback_transition_exception else "1")
            if callback_transition_exception:
                env["SKYAPO_TEST_TRANSITION_MS"] = "500"
            callback_exception_config.write_text(pathlib.Path(dsp_config).read_text())
            dsp_config = callback_exception_config
        pipewire_log = (root / "pipewire-server.log").open("w+")
        wireplumber_log = (root / "wireplumber-policy.log").open("w+")
        pw_cli_log = (root / "pw-cli-target-manager.log").open("w+")
        source_log = (root / "source.log").open("w+")
        daemon_log = (root / "daemon.log").open("w+")
        race_monitor_log = (root / "pdc-config-commit-monitor.log").open("w+")
        server = source_process = second_source_process = daemon_process = None
        unrelated_playback_process = None
        wireplumber_process = None
        wireplumber_paused = False
        null_target_manager = None
        playback_consumer = None
        post_crash_playback_consumer = None
        unrelated_playback_consumer = None
        transition_consumer = None
        rollback_consumer = None
        transition_fault_consumer = None
        boundary_consumer = None
        race_monitor = None
        config_reload_process = None
        playback_consumer_log = (root / "playback-consumer.log").open("w+")
        unrelated_playback_log = (root / "unrelated-playback.log").open("w+")
        try:
            server = start_private_pipewire(
                pipewire, rate_config, env, pipewire_log, runtime)
            if wireplumber_playback:
                wireplumber_process = start_wireplumber(
                    wireplumber, env, wireplumber_log)
                time.sleep(0.5)
                if wireplumber_process.poll() is not None:
                    raise RuntimeError(
                        "private WirePlumber policy process exited during startup")
                # Use PipeWire's real SPA null-audio-sink adapter as the
                # physical-output stand-in. A test pw_filter Audio/Sink is not
                # a device adapter and can stall WirePlumber's policy hook.
                target_properties = (
                    '{ factory.name = "support.null-audio-sink" '
                    'node.name = "skyapo.test.null-target" '
                    'node.description = "SkyAPO Test Playback Target" '
                    f'media.class = "Audio/Sink" audio.channels = {channel_count} '
                    f'audio.position = "[ {channel_positions} ]" node.virtual = true '
                    'monitor.channel-volumes = true }'
                )
                if stable_playback_target:
                    target_properties = target_properties.replace(
                        'node.virtual = true',
                        'device.serial = "SKYAPO-PLAYBACK-SERIAL-42" '
                        'node.virtual = true')
                null_target_manager = subprocess.Popen(
                    [str(pw_cli)], env=env, text=True, stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=pw_cli_log)
                null_target_manager.stdin.write(
                    f"create-node adapter {target_properties}\n")
                null_target_manager.stdin.flush()
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    objects = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    if any(
                            item.get("type", "").endswith(":Node") and
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name") == "skyapo.test.null-target"
                            for item in objects):
                        break
                    time.sleep(0.05)
                else:
                    graph = run([str(pw_dump)], env, timeout=5,
                                check=False)
                    graph_nodes = []
                    if graph.returncode == 0:
                        graph_nodes = [
                            {
                                "name": item.get("info", {}).get("props", item.get("props", {}))
                                .get("node.name"),
                                "class": item.get("info", {}).get("props", item.get("props", {}))
                                .get("media.class"),
                                "factory": item.get("info", {}).get("props", item.get("props", {}))
                                .get("factory.name"),
                            }
                            for item in pipewire_dump_objects(graph.stdout)
                            if item.get("type", "").endswith(":Node")
                        ]
                    raise RuntimeError(
                        "PipeWire did not create the SPA null playback target; "
                        f"pw-cli process status {null_target_manager.poll()}; "
                        f"pw-dump returned {graph.returncode}; nodes="
                        f"{graph_nodes}; error={graph.stderr}")
                if (output_switch or playback_isolation or recovery_playback_target
                        or playback_systemd_user_override):
                    alternate_properties = target_properties.replace(
                        'node.name = "skyapo.test.null-target"',
                        'node.name = "skyapo.test.null-target-alt"').replace(
                        'node.description = "SkyAPO Test Playback Target"',
                        'node.description = "SkyAPO Alternate Test Playback Target"')
                    if stable_playback_target:
                        alternate_properties = alternate_properties.replace(
                            'device.serial = "SKYAPO-PLAYBACK-SERIAL-42"',
                            'device.serial = "SKYAPO-PLAYBACK-ALT-SERIAL-43"')
                    create_null_sink(null_target_manager, alternate_properties)
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        objects = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        target_names = {
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name")
                            for item in objects
                            if item.get("type", "").endswith(":Node")}
                        if "skyapo.test.null-target-alt" in target_names:
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "PipeWire did not create the alternate playback target")

            if wireplumber_playback:
                metadata = shutil.which("pw-metadata")
                if not metadata:
                    raise RuntimeError(
                        "WirePlumber playback test requires pw-metadata")
                wpctl = shutil.which("wpctl")
                if not wpctl:
                    raise RuntimeError(
                        "WirePlumber playback test requires wpctl")
                graph = pipewire_dump_objects(
                    run([str(pw_dump)], env, timeout=5).stdout)
                initial_target_id = next((
                    str(item.get("id")) for item in graph
                    if item.get("type", "").endswith(":Node") and
                    item.get("info", {}).get("props", item.get("props", {}))
                    .get("node.name") == "skyapo.test.null-target"), None)
                if initial_target_id is None:
                    raise RuntimeError(
                        "cannot identify initial WirePlumber playback target")
                # Keep WirePlumber's selected target and PipeWire's default
                # metadata in sync. Writing only core metadata with pw-metadata
                # lets WirePlumber restore its separate fallback after shutdown.
                run([wpctl, "set-default", initial_target_id], env, timeout=5)
                deadline = time.monotonic() + 5
                default_metadata = ""
                expected_default_metadata = (
                    "value:'{\"name\":\"skyapo.test.null-target\"}'")
                while time.monotonic() < deadline:
                    default_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if expected_default_metadata in default_metadata:
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "WirePlumber did not confirm the initial playback target "
                        f"through wpctl:\n{default_metadata}")
                initial_configured_sink = metadata_node_name(
                    default_metadata, "default.configured.audio.sink")
                initial_default_sink = metadata_node_name(
                    default_metadata, "default.audio.sink")

            source_process = start_test_source(
                source, mono, env, source_log,
                serial="SKYAPO-TEST-SERIAL-42" if stable_id_replug else None,
                surround51=surround51, surround71=surround71,
                latency_probe=latency_probe,
                start_silent=pdc_format_event_order,
                pdc_trigger_staging=lv2_pdc_config_commit_race,
                link_node=("skyapo.playback" if mode == "playback-preamp"
                           else None))
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if source_process.poll() is not None:
                    raise RuntimeError("deterministic PipeWire source exited")
                devices = run([str(cli), "device", "list"], env, timeout=5)
                if device_name in devices.stdout:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("deterministic capture source was not enumerated")

            selected = run([str(cli), "device", "set", device_name], env)
            if device_name not in selected.stdout:
                raise RuntimeError(f"device selection did not persist: {selected.stdout}")
            if stable_id_replug:
                device_list = run([str(cli), "device", "list"], env).stdout
                pipewire_objects = pipewire_dump_objects(
                    run([str(pw_dump)], env).stdout)
                source_props = next((
                    item.get("info", {}).get("props", item.get("props", {}))
                    for item in pipewire_objects
                    if item.get("type", "").endswith(":Node") and
                    item.get("info", {}).get("props", item.get("props", {}))
                    .get("node.name") == device_name), {})
                if ("device.serial=SKYAPO-TEST-SERIAL-42" not in device_list or
                        not re.search(r"(?m)^\d+\t" + re.escape(device_name) +
                                      r"\t[^\n]*\tyes\t", device_list)):
                    raise RuntimeError(
                        "stable test identity was not advertised and persisted "
                        f"by device selection:\n{device_list}\n"
                        f"PipeWire test source properties: {source_props}")
            current = run([str(cli), "device", "current"], env)
            if device_name not in current.stdout:
                raise RuntimeError(f"selected source mismatch: {current.stdout}")

            if mode in ("playback-preamp", "playback-wireplumber",
                        "playback-wireplumber-volume",
                        "playback-wireplumber-stage",
                        "playback-wireplumber-mute",
                        "playback-wireplumber-target-recovery",
                        "playback-wireplumber-target-recovery-stable-id",
                        "playback-wireplumber-startup-recovery",
                        "playback-wireplumber-server-restart",
                        "playback-wireplumber-systemd-crash-restart",
                        "playback-wireplumber-systemd-crash-user-override",
                        "playback-wireplumber-recovery-write-failure",
                        "playback-wireplumber-route-confirmation-race",
                        "playback-wireplumber-output-switch",
                        "playback-wireplumber-surround51",
                        "playback-wireplumber-surround71",
                        "playback-wireplumber-lv2-latency",
                        "playback-wireplumber-pdc-recovery",
                        "playback-wireplumber-quantum-variation"):
                consumer_args = [str(consumer)]
                if surround51:
                    consumer_args.append("--surround51")
                elif surround71:
                    consumer_args.append("--surround71")
                if mode.endswith("-mute"):
                    consumer_args.append("--expect-silent")
                else:
                    expected_playback_db = "-18" if mode.endswith("-volume") else "-6"
                    consumer_args += ["--expected-db", expected_playback_db]
                consumer_args += [
                     "--expected-rate", str(sample_rate), "--duration-ms",
                     ("30000" if wireplumber_playback else "15000"),
                     "--capture-node",
                     ("skyapo.test.null-target" if wireplumber_playback else
                      "skyapo.playback.stream"),
                     "--ignore-silent-blocks"]
                if wireplumber_playback:
                    # This recorder is a test-only monitor consumer, not an
                    # audio device. A pw_filter Audio/Sink cannot implement
                    # WirePlumber 0.4's device PortConfig transaction and can
                    # stall policy for later real playback streams.
                    consumer_args.append("--internal-filter")
                if mode.endswith("-mute"):
                    consumer_args.remove("--ignore-silent-blocks")
                if playback_isolation:
                    consumer_args += ["--max-consecutive-silence-ms", "250"]
                if playback_pdc_recovery or playback_pdc_refresh:
                    consumer_args[
                        consumer_args.index("--duration-ms") + 1] = "5000"
                if playback_quantum_variation:
                    consumer_args[
                        consumer_args.index("--duration-ms") + 1] = "8000"
                    consumer_args.remove("--ignore-silent-blocks")
                if (not wireplumber_playback and
                        not startup_playback_recovery and
                        not playback_pdc_recovery and
                        not playback_quantum_variation):
                    playback_consumer = subprocess.Popen(
                        consumer_args,
                        env=env, text=True,
                        stdout=playback_consumer_log,
                        stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    sinks = run([str(cli), "device", "output", "list"], env,
                                timeout=5, check=False)
                    expected_sink = ("skyapo.test.null-target"
                                     if wireplumber_playback else
                                     "skyapo.test.consumer")
                    if expected_sink in sinks.stdout:
                        break
                    if (playback_consumer is not None and
                            playback_consumer.poll() is not None):
                        raise RuntimeError(
                            "playback test sink consumer exited before startup")
                    time.sleep(0.1)
                else:
                    raise RuntimeError("test playback sink was not enumerated")
                run([str(cli), "device", "output", "set",
                     expected_sink], env, timeout=5)
                if startup_playback_recovery:
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    target_id = next((
                        str(item.get("id")) for item in graph
                        if item.get("type", "").endswith(":Node") and
                        item.get("info", {}).get("props", item.get("props", {}))
                        .get("node.name") == "skyapo.test.null-target"), None)
                    if target_id is None:
                        raise RuntimeError(
                            "cannot remove selected playback target before "
                            "daemon startup")
                    run([str(pw_cli), "destroy", target_id], env, timeout=5)
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        graph = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        if not any(
                                item.get("type", "").endswith(":Node") and
                                item.get("info", {}).get("props", item.get("props", {}))
                                .get("node.name") == "skyapo.test.null-target"
                                for item in graph):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "selected playback target did not disappear before "
                            "daemon startup")

            if systemd_crash_restart:
                systemd_run = shutil.which("systemd-run")
                systemctl = shutil.which("systemctl")
                if not systemd_run or not systemctl:
                    raise RuntimeError(
                        "systemd-crash-restart requires systemd-run and systemctl")
                daemon_process = start_systemd_daemon(
                    systemd_run, systemctl, daemon, dsp_config, env,
                    os.environ.get("SKYAPO_SYSTEMD_UNIT_TEMPLATE", ""),
                    "10s" if playback_systemd_user_override else None)
            else:
                daemon_process = subprocess.Popen(
                    [str(daemon), "--config", str(dsp_config)], env=env,
                    stdout=daemon_log, stderr=subprocess.STDOUT)
            if startup_playback_recovery:
                deadline = time.monotonic() + 8
                initial_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    initial_status = result.stdout
                    if (result.returncode == 0 and
                            "Daemon: streaming" in initial_status and
                            "Playback graph: unavailable" in initial_status and
                            "selected playback sink unavailable: "
                            "skyapo.test.null-target" in initial_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "daemon did not remain capture-active while its saved "
                        "playback target was absent:\n" + initial_status)
                stage_config.write_text(
                    "Stage: capture\nPreamp: -3 dB\n"
                    "Stage: post-mix\nUnsupportedPlaybackStage: true\n")
                rejected_reload = run(
                    [str(cli), "config", "reload"], env, timeout=10,
                    check=False)
                if rejected_reload.returncode == 0:
                    raise RuntimeError(
                        "invalid post-mix config was accepted while the "
                        "playback graph was unavailable:\n"
                        f"{rejected_reload.stdout}{rejected_reload.stderr}")
                rejected_status = run(
                    [str(cli), "status"], env, timeout=5).stdout
                capture_ratio = re.search(
                    r"DSP amplitude ratio: ([0-9.]+)", rejected_status)
                if (not capture_ratio or
                        abs(float(capture_ratio.group(1)) - 0.501187) >= 0.01):
                    raise RuntimeError(
                        "rejected post-mix reload changed the last-good capture "
                        f"Engine while playback was unavailable:\n{rejected_status}")
                stage_config.write_text(
                    "Stage: capture\nPreamp: -6 dB\n"
                    "Stage: post-mix\nPreamp: -6 dB\n")
                create_null_sink(null_target_manager, target_properties)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    if any(
                            item.get("type", "").endswith(":Node") and
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name") == "skyapo.test.null-target"
                            for item in graph):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "selected playback target did not return after daemon "
                        "startup")
                metadata = shutil.which("pw-metadata")
                if not metadata:
                    raise RuntimeError(
                        "startup playback recovery requires pw-metadata")
                run([metadata, "-n", "default", "0", "default.audio.sink",
                     '{"name":"skyapo.test.null-target"}',
                     "Spa:String:JSON"], env, timeout=5)
            if wireplumber_playback:
                deadline = time.monotonic() + 8
                playback_nodes_ready = False
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited before publishing the smart-filter pair")
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    names = {
                        item.get("info", {}).get("props", item.get("props", {}))
                        .get("node.name") for item in graph
                        if item.get("type", "").endswith(":Node")}
                    if {"skyapo.playback", "skyapo.playback.stream"} <= names:
                        playback_nodes_ready = True
                        break
                    time.sleep(0.05)
                if not playback_nodes_ready:
                    raise RuntimeError(
                        "PipeWire did not publish the complete smart-filter pair")
                if mode.endswith(("-volume", "-mute")):
                    # The registry exposes the Audio/Sink before WirePlumber
                    # finishes PortConfig and selects it as the default. Wait
                    # for the runtime graph to own the route before setting
                    # Props, otherwise policy initialization can overwrite a
                    # control change made against the early node.
                    deadline = time.monotonic() + 8
                    playback_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        playback_status = result.stdout
                        if (result.returncode == 0 and
                                "Playback graph: streaming / streaming, "
                                "internal links 2/2, target links 2/2 channels"
                                in playback_status and
                                "PipeWire default sink: skyapo.playback" in
                                playback_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "WirePlumber playback graph did not own the "
                            "default route before control setup:\n"
                            f"{playback_status}")
                    playback_objects = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    controls = (
                        [("skyapo.playback",
                          '{"volume":0.5,"mute":false,'
                          '"channelVolumes":[0.5,0.5]}'),
                         ("skyapo.playback.stream",
                          '{"volume":0.5,"mute":false}')]
                        if mode.endswith("-volume") else
                        [("skyapo.playback.stream", '{"mute":true}')])
                    for control_node_name, props_json in controls:
                        control_node = next((
                            str(item.get("id")) for item in playback_objects
                            if item.get("type", "").endswith(":Node") and
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name") == control_node_name), None)
                        if control_node is None:
                            raise RuntimeError(
                                f"PipeWire graph lacks control node "
                                f"{control_node_name}")
                        changed = run([str(pw_cli), "set-param", control_node,
                                       "Props", props_json], env, timeout=5,
                                      check=False)
                        if changed.returncode != 0:
                            raise RuntimeError(
                                f"Could not set {control_node_name} Props: "
                                f"{changed.stdout}{changed.stderr}")
                time.sleep(0.25)
                if startup_playback_recovery:
                    playback_client_args = [str(playback_client)]
                    if surround51:
                        playback_client_args.append("--surround51")
                    elif surround71:
                        playback_client_args.append("--surround71")
                    second_source_process = subprocess.Popen(
                        playback_client_args, env=env, text=True,
                        stdout=source_log, stderr=subprocess.STDOUT)
                if playback_pdc_recovery:
                    # Start playback first so the selected sink monitor has a
                    # real PipeWire quantum to deliver. Keep measurement
                    # disarmed until the candidate commit barrier; this
                    # excludes valid pre-fault audio while retaining the first
                    # quantum processed by the unsafe candidate.
                    transition_fault_consumer = subprocess.Popen(
                        [str(consumer), "--signal-connected",
                         "--signal-ready",
                         "--arm-on-signal", "--expect-silent",
                         "--expected-rate",
                         str(sample_rate), "--duration-ms", "30000",
                         "--capture-node", "skyapo.test.null-target",
                         "--internal-filter"],
                        env=env, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
                    try:
                        transition_fault_ready_output = wait_for_consumer_ready(
                            transition_fault_consumer,
                            "first-fault playback monitor",
                            marker="SKYAPO_CONSUMER_READY")
                    except RuntimeError as error:
                        graph = run([str(pw_dump)], env, timeout=5,
                                    check=False)
                        graph_summary = (
                            summarize_graph_dump(graph.stdout)
                            if graph.returncode == 0 else
                            f"pw-dump failed ({graph.returncode}): "
                            f"{graph.stderr}")
                        raise RuntimeError(
                            f"{error}\n--- graph at first-fault monitor "
                            f"readiness failure ---\n{graph_summary}") from error
            if lv2_pdc_config_commit_race or delay_pdc or (lv2_pdc_recovery and
                             not playback_pdc_recovery):
                expected_filters = 4
            elif vst2_live or playback_pdc_recovery:
                expected_filters = 1
            elif lv2_pdc_final:
                expected_filters = 3
            else:
                expected_filters = 2 if plugin_chain else 1
            deadline = time.monotonic() + 15
            status = ""
            observed_dsp_initialization = False
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    raise RuntimeError("skyapod exited during startup")
                result = run([str(cli), "status"], env, timeout=5, check=False)
                status = result.stdout
                if (result.returncode == 0 and
                        "Daemon: streaming" in status and
                        "Sample rate: unknown (awaiting graph)" in status):
                    observed_dsp_initialization = True
                    uninitialized_status = (
                        "DSP state: initializing (awaiting negotiated format)",
                        "Filters: unknown (DSP engine not initialized)",
                        "  (DSP engine not initialized)")
                    if not all(value in status for value in uninitialized_status):
                        raise RuntimeError(
                            "PipeWire entered streaming before format negotiation, "
                            "but status misreported the DSP engine state:\n" + status)
                callback_exception_observed = (
                    "DSP state: paused (realtime processing exception; "
                    "output silence)" in status and
                    "DSP callback error: processing exception" in status)
                if (result.returncode == 0 and "Daemon: streaming" in status and
                        "SkyAPO Virtual Mic" in status and
                        f"Sample rate: {sample_rate} Hz" in status and
                        (callback_exception_observed
                         if callback_process_exception and
                         not callback_transition_exception
                         else (plugin_chain or delay_pdc or convolution or
                               ("DSP amplitude ratio: 1" if playback_stage else
                                "DSP amplitude ratio: 0.501187") in status)) and
                        f"Filters: {expected_filters}" in status):
                    if mode in ("playback-preamp", "playback-wireplumber",
                                "playback-wireplumber-volume",
                                "playback-wireplumber-stage",
                                "playback-wireplumber-mute",
                                "playback-wireplumber-target-recovery",
                                "playback-wireplumber-target-recovery-stable-id",
                                "playback-wireplumber-startup-recovery",
                                "playback-wireplumber-output-switch",
                                "playback-wireplumber-surround51",
                                "playback-wireplumber-surround71",
                                "playback-wireplumber-lv2-latency",
                        "playback-wireplumber-pdc-recovery",
                        "playback-wireplumber-quantum-variation") and (
                            "Playback graph: streaming / streaming, internal links "
                            f"{channel_count}/{channel_count}, target links "
                            f"{channel_count}/{channel_count} channels" not in status or
                            f"Playback filters: "
                            f"{1 if playback_pdc_recovery else (2 if playback_pdc_refresh else 1)}"
                            not in status):
                        time.sleep(0.1)
                        continue
                    break
                time.sleep(0.1)
            else:
                policy_graph = ""
                if wireplumber_playback:
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    policy_graph = "\nPrivate WirePlumber graph: " + repr([
                        item for item in graph
                        if item.get("type", "").endswith(":Link") or
                        (item.get("type", "").endswith(":Node") and
                         "skyapo" in repr(item))])
                raise RuntimeError(
                    f"daemon did not reach streaming state:\n{status}"
                    f"{policy_graph}")
            if wireplumber_playback:
                if playback_route_cleanup_failure:
                    expected_failure = (
                        "cannot confirm the PipeWire default sink update"
                        if playback_route_sync_failure else
                        "PipeWire/WirePlumber did not confirm SkyAPO as the "
                        "default playback route")
                    deadline = time.monotonic() + 6
                    route_status = ""
                    while time.monotonic() < deadline:
                        route_status = run(
                            [str(cli), "status"], env, timeout=5).stdout
                        if expected_failure in route_status:
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "injected route confirmation failure was not "
                            f"reported ({expected_failure}):\n"
                            + route_status)
                    changed_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if (metadata_node_name(
                            changed_metadata, "default.audio.sink") !=
                            "skyapo.playback" or
                            metadata_node_name(
                                changed_metadata,
                                "default.configured.audio.sink") !=
                            "skyapo.playback"):
                        raise RuntimeError(
                            "test did not reproduce accepted active and "
                            "configured SkyAPO route writes before confirmation "
                            "failure:\n"
                            + changed_metadata)
                    state_file = (state_home / "skyapo" /
                                  "playback-default-route-v1")
                    if not state_file.is_file():
                        raise RuntimeError(
                            "unconfirmed route change lost its recovery snapshot")
                    stopped = run([str(cli), "stop"], env, timeout=8)
                    if "Stopping skyapod" not in stopped.stdout:
                        raise RuntimeError(
                            "daemon did not acknowledge stop after route "
                            f"confirmation failure: {stopped.stdout}")
                    daemon_process.wait(timeout=5)
                    restored_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if (metadata_node_name(
                            restored_metadata, "default.audio.sink") !=
                            initial_default_sink or
                            metadata_node_name(
                                restored_metadata,
                                "default.configured.audio.sink") !=
                            initial_configured_sink):
                        raise RuntimeError(
                            "clean shutdown did not conditionally restore the "
                            "route after a core-sync/confirmation failure:\n" +
                            restored_metadata)
                    print("Accepted but unconfirmed active/configured route "
                          "writes were conditionally restored on clean "
                          "shutdown after the injected confirmation failure.")
                    return 0

                if playback_route_confirmation_race:
                    deadline = time.monotonic() + 6
                    route_status = ""
                    while time.monotonic() < deadline:
                        route_status = run(
                            [str(cli), "status"], env, timeout=5).stdout
                        if ("Playback routing policy: inactive "
                                "(PipeWire/WirePlumber did not confirm SkyAPO "
                                "as the default playback route)" in
                                route_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "route confirmation race was not kept fail-closed; "
                            "the route must remain inactive after its active "
                            "metadata is reverted before core sync:\n" +
                            route_status)
                    current_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if (metadata_node_name(
                            current_metadata, "default.audio.sink") !=
                            initial_default_sink):
                        raise RuntimeError(
                            "PipeWire active default changed despite the "
                            "pre-sync override:\n" + current_metadata)
                    if (metadata_node_name(
                            current_metadata,
                            "default.configured.audio.sink") !=
                            initial_default_sink):
                        raise RuntimeError(
                            "WirePlumber configured default changed despite "
                            "the pre-sync override:\n" + current_metadata)
                    print("A pre-sync default-route override invalidated the "
                          "stale active/configured confirmations; SkyAPO "
                          "stayed inactive.")
                    stopped = run([str(cli), "stop"], env, timeout=8)
                    if "Stopping skyapod" not in stopped.stdout:
                        raise RuntimeError(
                            "daemon did not acknowledge stop after the route "
                            f"confirmation race: {stopped.stdout}")
                    daemon_process.wait(timeout=5)
                    return 0

                if playback_recovery_write_failure:
                    if isinstance(daemon_process, SystemdManagedDaemon):
                        raise RuntimeError(
                            "recovery-write-failure requires a directly "
                            "managed daemon process")
                    deadline = time.monotonic() + 8
                    route_status = ""
                    while time.monotonic() < deadline:
                        route_status = run(
                            [str(cli), "status"], env, timeout=5).stdout
                        if ("Playback routing policy: active" in route_status and
                                "PipeWire default sink: skyapo.playback" in
                                route_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "initial daemon did not claim the playback route "
                            "before crash simulation:\n" + route_status)
                    state_file = (state_home / "skyapo" /
                                  "playback-default-route-v1")
                    if not state_file.is_file():
                        raise RuntimeError(
                            "initial playback route ownership did not persist "
                            "a crash-recovery snapshot")
                    daemon_process.send_signal(signal.SIGKILL)
                    daemon_process.wait(timeout=5)
                    daemon_process = None

                    # Model a crash after the configured route was persisted
                    # but before the active default was confirmed. Leave
                    # WirePlumber running so skyapod can build its PipeWire
                    # graph during recovery.
                    run([metadata, "-n", "default", "0",
                         "default.audio.sink",
                         json.dumps({"name": initial_default_sink}),
                         "Spa:String:JSON"], env, timeout=5)
                    partial_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if (metadata_node_name(
                            partial_metadata, "default.audio.sink") !=
                            initial_default_sink or
                            metadata_node_name(
                                partial_metadata,
                                "default.configured.audio.sink") !=
                            "skyapo.playback"):
                        raise RuntimeError(
                            "could not establish partial route recovery state "
                            f"after the crash; expected active="
                            f"{initial_default_sink!r}, configured="
                            "'skyapo.playback', got:\n"
                            f"{partial_metadata}")
                    pathlib.Path(
                        env["SKYAPO_TEST_FAIL_CONFIGURED_DEFAULT_WRITE_FILE"]
                    ).touch()
                    hold_recovery = pathlib.Path(
                        env["SKYAPO_TEST_HOLD_DEFAULT_ROUTE_RECOVERY_FILE"])
                    hold_recovery.touch()
                    daemon_process = subprocess.Popen(
                        [str(daemon), "--config", str(dsp_config)], env=env,
                        stdout=daemon_log, stderr=subprocess.STDOUT)
                    deadline = time.monotonic() + 12
                    recovery_graph_status = ""
                    while time.monotonic() < deadline:
                        if daemon_process.poll() is not None:
                            raise RuntimeError(
                                "skyapod exited before partial recovery setup")
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        recovery_graph_status = result.stdout
                        if (result.returncode == 0 and
                                "Daemon: streaming" in recovery_graph_status and
                                "Playback graph: streaming / streaming" in
                                recovery_graph_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "replacement daemon did not bring up playback "
                            "while route recovery was held:\n" +
                            recovery_graph_status)
                    run([metadata, "-n", "default", "0",
                         "default.audio.sink",
                         json.dumps({"name": initial_default_sink}),
                         "Spa:String:JSON"], env, timeout=5)
                    partial_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if (metadata_node_name(
                            partial_metadata, "default.audio.sink") !=
                            initial_default_sink or
                            metadata_node_name(
                                partial_metadata,
                                "default.configured.audio.sink") !=
                            "skyapo.playback"):
                        raise RuntimeError(
                            "replacement daemon could not hold the partial "
                            "recovery metadata state:\n" + partial_metadata)
                    os.killpg(wireplumber_process.pid, signal.SIGSTOP)
                    wireplumber_paused = True
                    hold_recovery.unlink()
                    deadline = time.monotonic() + 15
                    recovery_status = ""
                    while time.monotonic() < deadline:
                        if daemon_process.poll() is not None:
                            raise RuntimeError(
                                "skyapod exited during partial route recovery")
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        recovery_status = result.stdout
                        if (result.returncode == 0 and
                                "Daemon: streaming" in recovery_status and
                                "cannot update WirePlumber's configured "
                                "playback default: Input/output error" in
                                recovery_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "recovery write failure was not observed after "
                            "the simulated daemon crash:\n" + recovery_status)
                    os.killpg(wireplumber_process.pid, signal.SIGCONT)
                    wireplumber_paused = False

                if playback_route_write_failure or playback_recovery_write_failure:
                    deadline = time.monotonic() + 5
                    failure_status = ""
                    while time.monotonic() < deadline:
                        failure_status = run(
                            [str(cli), "status"], env, timeout=5).stdout
                        if ("cannot update WirePlumber's configured playback "
                                "default: Input/output error" in failure_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "the configured-default write failure was not "
                            "observed in runtime status:\n" + failure_status)

                    deadline = time.monotonic() + 5
                    restored_metadata = ""
                    while time.monotonic() < deadline:
                        restored_metadata = run(
                            [metadata, "-n", "default"], env,
                            timeout=5).stdout
                        if (metadata_node_name(
                                restored_metadata, "default.audio.sink") ==
                                initial_default_sink):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "failed configured-default write left the active "
                            "PipeWire sink partially changed; expected rollback "
                            f"to {initial_default_sink!r}, got:\n"
                            f"{restored_metadata}")
                    expected_configured_sink = (
                        "skyapo.playback" if playback_recovery_write_failure
                        else initial_configured_sink)
                    if (metadata_node_name(
                            restored_metadata,
                            "default.configured.audio.sink") !=
                            expected_configured_sink):
                        raise RuntimeError(
                            "failed configured-default write changed the "
                            "expected "
                            "WirePlumber route:\n" + restored_metadata)
                    if playback_recovery_write_failure:
                        if ("Playback routing policy: inactive (cannot update "
                                "WirePlumber's configured playback default: "
                                "Input/output error" not in failure_status or
                                "Playback routing policy: active" in
                                failure_status):
                            raise RuntimeError(
                                "recovery write failure was reported as active:\n"
                                + failure_status)
                        state_file = (state_home / "skyapo" /
                                      "playback-default-route-v1")
                        if not state_file.is_file():
                            raise RuntimeError(
                                "failed route recovery removed its snapshot")
                        if pathlib.Path(
                                env["SKYAPO_TEST_FAIL_CONFIGURED_DEFAULT_WRITE_FILE"]
                                ).exists():
                            raise RuntimeError(
                                "one-shot recovery write failure was not consumed")
                        stopped = run([str(cli), "stop"], env, timeout=8)
                        if "Stopping skyapod" not in stopped.stdout:
                            raise RuntimeError(
                                "daemon did not acknowledge stop after failed "
                                f"route recovery: {stopped.stdout}")
                        daemon_process.wait(timeout=5)
                        restored_metadata = run(
                            [metadata, "-n", "default"], env,
                            timeout=5).stdout
                        if (metadata_node_name(
                                restored_metadata, "default.audio.sink") !=
                                initial_default_sink or
                                metadata_node_name(
                                    restored_metadata,
                                    "default.configured.audio.sink") !=
                                initial_configured_sink):
                            raise RuntimeError(
                                "clean stop did not restore the still-owned "
                                "configured route after recovery write failure:\n"
                                + restored_metadata)
                        if state_file.exists():
                            raise RuntimeError(
                                "clean stop restored the route but left a stale "
                                f"recovery snapshot: {state_file}")
                        print("partial route recovery write failure left the "
                              "route inactive; clean shutdown restored the "
                              "remaining configured default and removed the "
                              "recovery snapshot")
                        return 0
                    print("failed configured-default write preserved both "
                          "original playback routes after WirePlumber recovery")
                    return 0
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and (
                        "PipeWire default sink: skyapo.playback" not in status):
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited before enabling the SkyAPO default route")
                    time.sleep(0.05)
                    status = run([str(cli), "status"], env, timeout=5).stdout
                if "PipeWire default sink: skyapo.playback" not in status:
                    raise RuntimeError(
                        "PipeWire did not route the system default through SkyAPO "
                        f"within 5 seconds:\n{status}")
                if stable_playback_target:
                    default_metadata = run(
                        [metadata, "-n", "default"], env, timeout=5).stdout
                    if metadata_node_name(
                            default_metadata,
                            "default.configured.audio.sink") != "skyapo.playback":
                        raise RuntimeError(
                            "WirePlumber's configured playback default did not "
                            "follow SkyAPO; hotplug can bypass DSP:\n"
                            f"{default_metadata}")
                if startup_playback_recovery:
                    wait_for_playback_application_route(
                        second_source_process, pw_dump, env, channel_count)
                    consumer_args[
                        consumer_args.index("--duration-ms") + 1] = "5000"
                    playback_consumer = subprocess.Popen(
                        consumer_args, env=env, text=True,
                        stdout=playback_consumer_log,
                        stderr=subprocess.STDOUT)
                if not playback_isolation and not startup_playback_recovery:
                    playback_client_args = [str(playback_client)]
                    if surround51:
                        playback_client_args.append("--surround51")
                    elif surround71:
                        playback_client_args.append("--surround71")
                    second_source_process = subprocess.Popen(
                        playback_client_args, env=env, text=True,
                        stdout=source_log, stderr=subprocess.STDOUT)
                    # Do not let playback assertions pass while the ordinary
                    # app is still linked directly to the old sink. Confirm
                    # that each channel reaches the SkyAPO policy adapter.
                    wait_for_playback_application_route(
                        second_source_process, pw_dump, env, channel_count)
                    if wireplumber_playback:
                        if isinstance(daemon_process, SystemdManagedDaemon):
                            startup_log = daemon_process.journal()
                        else:
                            daemon_log.flush()
                            daemon_log.seek(0)
                            startup_log = daemon_log.read()
                        manager_ready = (
                            "initial PipeWire registry synchronized; "
                            "session manager detected before playback adapter "
                            "creation" in startup_log or
                            "created playback adapter after the initial "
                            "session-manager discovery grace period; session manager "
                            "detected" in startup_log)
                        if not manager_ready:
                            raise RuntimeError(
                                "policy-managed playback route became active, but "
                                "skyapod created its playback adapter before "
                                "detecting the session manager:\n" + startup_log)
                        print("PipeWire session-manager discovery completed before the "
                              "SkyAPO playback adapter was exposed.")
                if playback_isolation:
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    alternate_target = next((
                        item for item in graph
                        if item.get("type", "").endswith(":Node") and
                        item.get("info", {}).get(
                            "props", item.get("props", {})).get(
                                "node.name") ==
                        "skyapo.test.null-target-alt"), None)
                    alternate_serial = (
                        alternate_target.get("info", {}).get(
                            "props", alternate_target.get("props", {})).get(
                                "object.serial")
                        if alternate_target else None)
                    if alternate_serial is None:
                        raise RuntimeError(
                            "PipeWire did not expose the alternate target's "
                            "object.serial")
                    playback_client_args = [str(playback_client)]
                    if surround51:
                        playback_client_args.append("--surround51")
                    elif surround71:
                        playback_client_args.append("--surround71")
                    # Start a normal application only after SkyAPO has become
                    # the confirmed default sink. Otherwise WirePlumber
                    # correctly connects it to the old default, and that
                    # existing stream does not follow a later default change.
                    second_source_process = subprocess.Popen(
                        playback_client_args, env=env, text=True,
                        stdout=source_log, stderr=subprocess.STDOUT)
                    unrelated_playback_process = subprocess.Popen(
                        [str(playback_client), "--target",
                         str(alternate_serial), "--name",
                         "skyapo.test.unrelated-playback-app"],
                        env=env, text=True, stdout=source_log,
                        stderr=subprocess.STDOUT)
                    unrelated_playback_consumer = subprocess.Popen(
                        [str(consumer), "--expected-db", "0",
                         "--expected-rate", str(sample_rate),
                         "--duration-ms", "5000", "--capture-node",
                         "skyapo.test.null-target-alt", "--name",
                         "skyapo.test.unrelated.consumer",
                         "--internal-filter"],
                        env=env, text=True, stdout=unrelated_playback_log,
                        stderr=subprocess.STDOUT)
                if (not startup_playback_recovery and
                        not playback_pdc_recovery and
                        not playback_quantum_variation):
                    playback_consumer = subprocess.Popen(
                        consumer_args, env=env, text=True,
                        stdout=playback_consumer_log,
                        stderr=subprocess.STDOUT)
            if observed_dsp_initialization:
                print("Observed the DSP initialization status transition.")
            else:
                print("DSP negotiated before the first status poll.")
            # The VST3 exception may be reached before or after this status
            # poll, depending on when PipeWire first schedules the source.
            # Assert its exact allocation count only after the independent
            # recorder has consumed the stream below.
            callback_allocations = None if vst3_exception_failure else 0
            expected_status = [
                f"Channels: {channel_count}", f"Filters: {expected_filters}",
                f"Channel positions: {channel_positions}",
                f"Active capture links: {channel_count}/{channel_count}",
                "Format: F32 planar DSP", f"Sample rate: {sample_rate} Hz",
                "Quantum: 1024",
                *([] if callback_allocations is None else [
                    f"Callback allocations: {callback_allocations}",
                    f"Callback deallocations: {callback_allocations}"]),
                (("Loudness volume endpoint: " +
                  ("skyapo.test.null-target (effective 0 dB)"
                   if wireplumber_playback else
                   "skyapo.test.consumer (effective 0 dB)"))
                 if mode in ("playback-preamp", "playback-wireplumber",
                             "playback-wireplumber-volume",
                             "playback-wireplumber-stage",
                             "playback-wireplumber-mute",
                             "playback-wireplumber-target-recovery",
                             "playback-wireplumber-target-recovery-stable-id",
                             "playback-wireplumber-startup-recovery",
                             "playback-wireplumber-server-restart",
                             "playback-wireplumber-systemd-crash-restart",
                             "playback-wireplumber-systemd-crash-user-override",
                             "playback-wireplumber-output-switch",
                             "playback-wireplumber-surround51",
                             "playback-wireplumber-surround71",
                             "playback-wireplumber-lv2-latency",
                             "playback-wireplumber-pdc-recovery",
                             "playback-wireplumber-quantum-variation") else
                 "Loudness volume endpoint: unavailable")]
            if stage_warning:
                expected_status.append(
                    "Config warnings:\n  " + str(dsp_config) + ":1: "
                    "pre-mix processing is unavailable")
            if latency_plugin:
                expected_status.append(
                    "Plugin-reported latency sum: 64 samples "
                    "(sum only; not end-to-end latency)")
            if lv2_pdc_final:
                expected_status.append(
                    "Plugin latency compensation: active "
                    "(plugin-reported latency only)")
            for expected in expected_status:
                if expected not in status:
                    raise RuntimeError(f"missing runtime value {expected!r}:\n{status}")
            if (wireplumber_playback and
                    "PipeWire default sink: skyapo.playback" not in status):
                raise RuntimeError(
                    "ordinary application playback is not routed through the "
                    "SkyAPO virtual sink while the daemon is active:\n" + status)
            if (not plugin_chain and not delay_pdc and not convolution and
                    not callback_process_exception and
                    ("DSP amplitude ratio: 1" if playback_stage else
                     "DSP amplitude ratio: 0.501187") not in status):
                raise RuntimeError(f"unexpected DSP amplitude ratio:\n{status}")
            if playback_stage:
                capture_ratio = re.search(
                    r"DSP amplitude ratio: ([0-9.]+)", status)
                if (not capture_ratio or
                        abs(float(capture_ratio.group(1)) - 1.0) >= 0.01):
                    raise RuntimeError(
                        "post-mix config branch changed capture samples; "
                        f"expected unity, status was:\n{status}")
            # Runtime RMS is cumulative from the instant the PipeWire graph
            # starts, so it includes initial Delay-line silence. The actual
            # steady-state PDC assertion is made by the independent consumer
            # below over a multi-second recording.

            if (vst3_latency_change or vst2_latency_change or
                    clap_latency_change or lv2_latency_change):
                plugin_format = ("VST3" if vst3_latency_change else
                                 ("VST2" if vst2_latency_change else
                                  ("CLAP" if clap_latency_change else "LV2")))
                notification_name = ("kLatencyChanged" if vst3_latency_change
                                     else ("audioMasterIOChanged" if
                                           vst2_latency_change else
                                           ("host.latency.changed" if
                                            clap_latency_change else
                                            "LV2 latency output")))
                deadline = time.monotonic() + 8
                latency_status = status
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            f"skyapod exited during {plugin_format} latency refresh")
                    result = run([str(cli), "status"], env, check=False)
                    latency_status = result.stdout
                    expected_latency = ("Plugin-reported latency sum: 32 samples "
                                        if lv2_latency_change else
                                        "Plugin-reported latency sum: 64 samples ")
                    if (result.returncode == 0 and
                            (expected_latency +
                             "(sum only; not end-to-end latency)")
                            in latency_status and
                            "Plugin latency refreshes: 1" in latency_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        f"{plugin_format} {notification_name} did not refresh the runtime "
                        f"latency snapshot:\n{latency_status}")
                if "Plugin latency update error:" in latency_status:
                    raise RuntimeError(
                        f"{plugin_format} latency refresh reported an error:\n"
                        f"{latency_status}")
                status = latency_status
                print(f"{plugin_format} latency output refreshed the live "
                      "snapshot on the control loop.")

            old_virtual_id = virtual_source_id(status)
            old_capture_id = re.search(r"^Capture node: (\d+)", status,
                                       re.MULTILINE)
            if not old_capture_id:
                raise RuntimeError("daemon status lacks initial capture node ID")
            assert_virtual_source(pw_dump, env, old_virtual_id)

            if mode in ("playback-preamp", "playback-wireplumber",
                        "playback-wireplumber-volume",
                        "playback-wireplumber-stage",
                        "playback-wireplumber-mute",
                        "playback-wireplumber-target-recovery",
                        "playback-wireplumber-target-recovery-stable-id",
                        "playback-wireplumber-startup-recovery",
                        "playback-wireplumber-output-switch",
                        "playback-wireplumber-surround51",
                        "playback-wireplumber-surround71",
                        "playback-wireplumber-lv2-latency",
                        "playback-wireplumber-pdc-recovery",
                        "playback-wireplumber-quantum-variation"):
                playback_objects = pipewire_dump_objects(
                    run([str(pw_dump)], env).stdout)
                playback_node = next((
                    str(item.get("id")) for item in playback_objects
                    if item.get("type", "").endswith(":Node") and
                    item.get("info", {}).get("props", item.get("props", {}))
                    .get("node.name") == "skyapo.playback"), None)
                if playback_node is None:
                    raise RuntimeError(
                        "PipeWire graph lacks the SkyAPO playback node")
                playback_stream_props = next((
                    item.get("info", {}).get("props", item.get("props", {}))
                    for item in playback_objects
                    if item.get("type", "").endswith(":Node") and
                    item.get("info", {}).get("props", item.get("props", {}))
                    .get("node.name") == "skyapo.playback.stream"), None)
                if (not playback_stream_props or
                        playback_stream_props.get("node.autoconnect") is not False or
                        playback_stream_props.get("node.dont-fallback") is not True or
                        playback_stream_props.get("node.linger") is not True):
                    raise RuntimeError(
                        "SkyAPO playback output stream must use explicit target "
                        "links, wait for its selected sink, and linger; "
                        f"properties={playback_stream_props}")
                advertised_formats = run(
                    [str(pw_cli), "enum-params", playback_node, "EnumFormat"],
                    env, timeout=5, check=False)
                if (advertised_formats.returncode != 0 or
                        "F32P" not in advertised_formats.stdout):
                    raise RuntimeError(
                        "SkyAPO playback node does not advertise its raw audio "
                        "format to PipeWire session managers:\n"
                        f"{advertised_formats.stdout}{advertised_formats.stderr}")
                if wireplumber_playback:
                    deadline = time.monotonic() + 8
                    expected_link = False
                    while time.monotonic() < deadline:
                        graph = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        nodes, links = {}, set()
                        node_props = {}
                        for item in graph:
                            info = item.get("info", {})
                            props = info.get("props", item.get("props", {}))
                            if item.get("type", "").endswith(":Node"):
                                name = props.get("node.name")
                                if name in ("skyapo.test.playback-app",
                                            "skyapo.test.unrelated-playback-app",
                                            "skyapo.playback",
                                            "skyapo.playback.stream",
                                            "skyapo.test.null-target",
                                            "skyapo.test.null-target-alt"):
                                    nodes[name] = str(item.get("id"))
                                    node_props[name] = props
                            elif item.get("type", "").endswith(":Link"):
                                links.add((
                                    str(props.get("link.output.node", "")),
                                    str(props.get("link.input.node", ""))))
                        expected_link = (
                            "skyapo.test.playback-app" in nodes and
                            "skyapo.playback" in nodes and
                            (nodes["skyapo.test.playback-app"],
                             nodes["skyapo.playback"]) in links)
                        unrelated_link = (
                            not playback_isolation or
                            ("skyapo.test.unrelated-playback-app" in nodes and
                             "skyapo.test.null-target-alt" in nodes and
                             (nodes["skyapo.test.unrelated-playback-app"],
                              nodes["skyapo.test.null-target-alt"]) in links))
                        if expected_link and unrelated_link:
                            break
                        if second_source_process.poll() is not None:
                            raise RuntimeError(
                                "WirePlumber playback test stream exited")
                        time.sleep(0.1)
                    if not expected_link:
                        raise RuntimeError(
                            "WirePlumber did not autoconnect an ordinary "
                            "playback stream to SkyAPO Playback; "
                            f"nodes={node_props}, links={links}")
                    if playback_isolation:
                        link_fault_trigger.write_text(
                            "fail next internal link creation\n")
                        removed_link = destroy_playback_internal_link(
                            pw_cli, pw_dump, env)
                        fault_deadline = time.monotonic() + 5
                        while (time.monotonic() < fault_deadline and
                               not link_fault_receipt.exists()):
                            time.sleep(0.02)
                        link_fault_trigger.unlink(missing_ok=True)
                        if not link_fault_receipt.exists():
                            raise RuntimeError(
                                "internal playback link creation failure was not "
                                "injected; recovery retry was not exercised")
                        if (playback_link_async_error and
                                link_fault_receipt.read_text().strip() !=
                                "observed asynchronous PipeWire link error"):
                            raise RuntimeError(
                                "PipeWire did not report the injected invalid-link "
                                "proxy through the asynchronous error callback")
                        deadline = time.monotonic() + 8
                        recovered_status = ""
                        expected_internal = (
                            "Playback graph: streaming / streaming, internal "
                            f"links {channel_count}/{channel_count}, target "
                            f"links {channel_count}/{channel_count} channels")
                        while time.monotonic() < deadline:
                            if daemon_process.poll() is not None:
                                raise RuntimeError(
                                    "skyapod exited after its internal playback "
                                    f"link {removed_link} was removed")
                            result = run([str(cli), "status"], env, timeout=5,
                                         check=False)
                            recovered_status = result.stdout
                            if (result.returncode == 0 and
                                    expected_internal in recovered_status and
                                    "Playback error:" not in recovered_status):
                                break
                            time.sleep(0.05)
                        else:
                            raise RuntimeError(
                                "SkyAPO did not recreate the removed internal "
                                f"playback link {removed_link}:\n"
                                f"{recovered_status}")
                        if ("Callback allocations: 0" not in recovered_status or
                                "Callback deallocations: 0" not in recovered_status):
                            raise RuntimeError(
                                "internal playback-link recovery violated the "
                                "realtime allocation invariant:\n"
                                f"{recovered_status}")
                        status = recovered_status
                        print("Playback graph recovered after PipeWire removed "
                              f"internal link {removed_link}.")
                        if playback_link_async_error:
                            # The long-running consumer above spans startup,
                            # fault injection, and recovery. Its aggregate RMS
                            # can dilute a brief dropout, so make a fresh
                            # independent capture after the recovered graph is
                            # reported healthy and verify the processed level
                            # in that isolated window as well.
                            post_recovery_args = [
                                str(consumer), "--expected-db", "-6",
                                "--expected-rate", str(sample_rate),
                                "--duration-ms", "2000", "--capture-node",
                                "skyapo.test.null-target", "--name",
                                "skyapo.test.consumer.post-recovery",
                                "--internal-filter"]
                            post_recovery = run(
                                post_recovery_args, env, timeout=8)
                            post_recovery_ratio = re.search(
                                r"RMS ratio to expected: ([0-9.]+)",
                                post_recovery.stdout)
                            if (not post_recovery_ratio or
                                    abs(float(post_recovery_ratio.group(1)) -
                                        1.0) >= 0.03):
                                raise RuntimeError(
                                    "independent post-recovery playback capture "
                                    "did not measure the expected -6 dB DSP "
                                    f"output:\n{post_recovery.stdout}")
                            print("Independent post-recovery playback capture "
                                  "verified the expected -6 dB level; measured "
                                  "output/expected ratio "
                                  f"{float(post_recovery_ratio.group(1)):.6f}.")
                    if playback_quantum_variation:
                        metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                                    shutil.which("pw-metadata"))
                        if not metadata:
                            raise RuntimeError(
                                "pw-metadata is required for playback quantum variation")
                        consumer_args[consumer_args.index("--duration-ms") + 1] = "12000"
                        time.sleep(0.25)
                        playback_consumer = subprocess.Popen(
                            consumer_args, env=env, text=True,
                            stdout=playback_consumer_log,
                            stderr=subprocess.STDOUT)

                        def playback_engine_build_count():
                            daemon_log.flush()
                            daemon_log.seek(0)
                            count = sum(
                                "skyapod: playback DSP ready at" in line
                                for line in daemon_log)
                            daemon_log.seek(0, os.SEEK_END)
                            return count

                        initial_builds = playback_engine_build_count()
                        quantum_sequence = (512, 1024, 256, 1024) * 4
                        for quantum in quantum_sequence:
                            run([metadata, "-n", "settings", "0",
                                 "clock.force-quantum", str(quantum),
                                 "Spa:Int"], env, timeout=5)
                            deadline = time.monotonic() + 5
                            expected_format = (
                                f"Playback format: {sample_rate} Hz, "
                                f"{channel_count} channels, quantum {quantum}")
                            while time.monotonic() < deadline:
                                current = run([str(cli), "status"], env,
                                              timeout=5, check=False)
                                if current.returncode == 0 and expected_format in current.stdout:
                                    break
                                time.sleep(0.02)
                            else:
                                raise RuntimeError(
                                    "playback graph did not observe forced PipeWire "
                                    f"quantum {quantum}:\n{current.stdout}")
                            if playback_engine_build_count() != initial_builds:
                                raise RuntimeError(
                                    "a smaller PipeWire block unnecessarily rebuilt "
                                    "the playback DSP engine instead of using its "
                                    f"existing capacity (quantum {quantum})")
                        print("Playback graph handled changing PipeWire block sizes "
                              "without rebuilding its DSP engine.")
                    if playback_isolation:
                        if not unrelated_link:
                            raise RuntimeError(
                                "an explicitly targeted unrelated playback stream "
                                "did not stay connected to its own sink; "
                                f"nodes={node_props}, links={links}")
                        unrelated_node = nodes[
                            "skyapo.test.unrelated-playback-app"]
                        if ((unrelated_node, nodes["skyapo.playback"]) in links or
                                (unrelated_node,
                                 nodes["skyapo.test.null-target"]) in links):
                            raise RuntimeError(
                                "unrelated playback was intercepted by the "
                                "selected SkyAPO sink")

                        adjacency = {name: set() for name in nodes}
                        id_to_name = {node_id: name
                                      for name, node_id in nodes.items()}
                        for output_id, input_id in links:
                            output_name = id_to_name.get(output_id)
                            input_name = id_to_name.get(input_id)
                            if output_name and input_name:
                                adjacency[output_name].add(input_name)
                        visiting, visited = set(), set()

                        def has_cycle(name):
                            if name in visiting:
                                return True
                            if name in visited:
                                return False
                            visiting.add(name)
                            if any(has_cycle(child)
                                   for child in adjacency[name]):
                                return True
                            visiting.remove(name)
                            visited.add(name)
                            return False

                        if any(has_cycle(name) for name in adjacency):
                            raise RuntimeError(
                                "active playback routing contains a feedback "
                                f"cycle: nodes={node_props}, links={links}")
                        print("Playback graph is acyclic; unrelated playback "
                              "stays on its explicitly selected sink.")
                    if recovery_playback_target:
                        objects = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        old_target_id = next((
                            str(item.get("id")) for item in objects
                            if item.get("type", "").endswith(":Node") and
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name") == "skyapo.test.null-target"), None)
                        if old_target_id is None:
                            raise RuntimeError("cannot identify initial playback target")
                        run([str(pw_cli), "destroy", old_target_id], env, timeout=5)
                        deadline = time.monotonic() + 8
                        unavailable_status = ""
                        while time.monotonic() < deadline:
                            if daemon_process.poll() is not None:
                                raise RuntimeError(
                                    "skyapod exited when its playback target was removed")
                            result = run([str(cli), "status"], env, timeout=5,
                                         check=False)
                            unavailable_status = result.stdout
                            if ("Playback target: skyapo.test.null-target (unavailable)"
                                    in unavailable_status and
                                    "Playback graph: streaming / streaming, internal links "
                                    "2/2, target links 0/2 channels" in unavailable_status):
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError(
                                "playback graph did not report its removed sink as unavailable:\n"
                                f"{unavailable_status}")
                        deadline = time.monotonic() + 3
                        preserved_filter_route = False
                        missing_target_graph = ""
                        while time.monotonic() < deadline:
                            graph = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5).stdout)
                            graph_nodes = {}
                            graph_links = set()
                            for item in graph:
                                info = item.get("info", {})
                                props = info.get("props", item.get("props", {}))
                                if item.get("type", "").endswith(":Node"):
                                    name = props.get("node.name")
                                    if name in ("skyapo.test.playback-app",
                                                "skyapo.playback",
                                                "skyapo.test.null-target-alt"):
                                        graph_nodes[name] = str(item.get("id"))
                                elif item.get("type", "").endswith(":Link"):
                                    graph_links.add((
                                        str(props.get("link.output.node", "")),
                                        str(props.get("link.input.node", ""))))
                            missing_target_graph = (
                                f"nodes={graph_nodes}, links={graph_links}")
                            app_node = graph_nodes.get("skyapo.test.playback-app")
                            filter_node = graph_nodes.get("skyapo.playback")
                            alternate_node = graph_nodes.get(
                                "skyapo.test.null-target-alt")
                            if app_node and filter_node and alternate_node:
                                preserved_filter_route = (
                                    (app_node, filter_node) in graph_links and
                                    (app_node, alternate_node) not in graph_links)
                                if preserved_filter_route:
                                    break
                            time.sleep(0.05)
                        if not preserved_filter_route:
                            raise RuntimeError(
                                "ordinary playback left the SkyAPO filter when its "
                                "selected target disappeared (audio may bypass DSP); "
                                f"{missing_target_graph}")
                        stop(playback_consumer, "pre-removal playback consumer")
                        playback_consumer = None
                        playback_consumer_log.seek(0)
                        playback_consumer_log.truncate(0)
                        fallback_consumer_args = [
                            str(consumer), "--expect-silent", "--expected-rate",
                            str(sample_rate), "--duration-ms", "1500",
                            "--capture-node", "skyapo.test.null-target-alt",
                            "--name", "skyapo.test.removed-target-fallback-monitor"]
                        playback_consumer = subprocess.Popen(
                            fallback_consumer_args, env=env, text=True,
                            stdout=playback_consumer_log,
                            stderr=subprocess.STDOUT)
                        try:
                            playback_consumer.wait(timeout=10)
                        except subprocess.TimeoutExpired as error:
                            raise RuntimeError(
                                "alternate-sink monitor did not finish while the "
                                "selected playback target was absent") from error
                        playback_consumer_log.flush()
                        playback_consumer_log.seek(0)
                        fallback_audio = playback_consumer_log.read()
                        fallback_rms = re.search(r"RMS: ([0-9.]+)", fallback_audio)
                        if (playback_consumer.returncode != 0 or not fallback_rms or
                                float(fallback_rms.group(1)) >= 1.0e-6):
                            raise RuntimeError(
                                "audio was redirected to the alternate sink after "
                                "the selected sink disappeared; expected fail-closed "
                                f"silence:\n{fallback_audio}\n{unavailable_status}")
                        print("Selected playback target removal kept the alternate "
                              "sink silent while the ordinary app stream remained "
                              f"active (RMS {float(fallback_rms.group(1)):.8f}).")
                        playback_consumer = None
                        playback_consumer_log.seek(0)
                        playback_consumer_log.truncate(0)
                        replacement_properties = target_properties.replace(
                            'node.name = "skyapo.test.null-target"',
                            f'node.name = "{replacement_target_name}"')
                        null_target_manager.stdin.write(
                            f"create-node adapter {replacement_properties}\n")
                        null_target_manager.stdin.flush()
                        deadline = time.monotonic() + 12
                        rebound_status = ""
                        rebound_target_id = None
                        while time.monotonic() < deadline:
                            objects = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5).stdout)
                            rebound_target_id = next((
                                str(item.get("id")) for item in objects
                                if item.get("type", "").endswith(":Node") and
                                item.get("info", {}).get("props", item.get("props", {}))
                                .get("node.name") == replacement_target_name), None)
                            result = run([str(cli), "status"], env, timeout=5,
                                         check=False)
                            rebound_status = result.stdout
                            if (rebound_target_id and rebound_target_id != old_target_id and
                                    f"Playback target: {replacement_target_name} (" in rebound_status and
                                    "Playback graph: streaming / streaming, internal links "
                                    "2/2, target links 2/2 channels" in rebound_status and
                                    "Playback error:" not in rebound_status):
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError(
                                "playback graph did not rebind to the recreated selected sink:\n"
                                f"{rebound_status}")
                        if stable_playback_target:
                            default_metadata = run(
                                [metadata, "-n", "default"], env,
                                timeout=5).stdout
                            if metadata_node_name(
                                    default_metadata,
                                    "default.configured.audio.sink") != "skyapo.playback":
                                raise RuntimeError(
                                    "WirePlumber lost SkyAPO as the configured "
                                    "default during target recovery; playback "
                                    "could bypass DSP:\n" + default_metadata)
                        wait_for_playback_application_route(
                            second_source_process, pw_dump, env, channel_count)
                        recovery_consumer_args = consumer_args.copy()
                        recovery_consumer_args[
                            recovery_consumer_args.index("--duration-ms") + 1] = "5000"
                        recovery_consumer_args[
                            recovery_consumer_args.index("--capture-node") + 1] = (
                                replacement_target_name)
                        playback_consumer = subprocess.Popen(
                            recovery_consumer_args, env=env, text=True,
                            stdout=playback_consumer_log,
                            stderr=subprocess.STDOUT)
                        playback_consumer.wait(timeout=10)
                        playback_consumer_log.flush()
                        playback_consumer_log.seek(0)
                        recovery_result = playback_consumer_log.read()
                        recovery_ratio = re.search(
                            r"RMS ratio to expected: ([0-9.]+)", recovery_result)
                        if (playback_consumer.returncode != 0 or not recovery_ratio or
                                abs(float(recovery_ratio.group(1)) - 1.0) >= 0.03):
                            recovery_graph = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5,
                                    check=False).stdout)
                            raise RuntimeError(
                                "independent playback recording failed after target recovery:\n"
                                f"{recovery_result}\n{rebound_status}\n"
                                f"PipeWire graph: "
                                f"{summarize_graph_dump(json.dumps(recovery_graph))}")
                        print("Selected PipeWire playback sink removal/recreation recovered "
                              f"target node {old_target_id} -> {rebound_target_id}; "
                              "independent monitor recording ratio to expected: "
                              f"{float(recovery_ratio.group(1)):.6f}.")
                    elif output_switch:
                        objects = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        old_playback_node = next((
                            str(item.get("id")) for item in objects
                            if item.get("type", "").endswith(":Node") and
                            item.get("info", {}).get("props", item.get("props", {}))
                            .get("node.name") == "skyapo.playback"), None)
                        if old_playback_node is None:
                            raise RuntimeError(
                                "cannot identify playback node before live target switch")
                        stop(playback_consumer, "pre-restart playback consumer")
                        playback_consumer = None
                        playback_consumer_log.seek(0)
                        playback_consumer_log.truncate(0)
                        if ui_test is not None:
                            ui_env = env.copy()
                            ui_env["QT_QPA_PLATFORM"] = "offscreen"
                            selected_output = run(
                                [str(ui_test), "--device-selector-live",
                                 str(dsp_config), str(cli),
                                 "skyapo.test.null-target-alt"],
                                ui_env, timeout=45)
                            if ("through skyapo, retained capture node" not in
                                    selected_output.stdout):
                                raise RuntimeError(
                                    "Device Selector UI did not report a verified "
                                    "live selection:\n"
                                    f"{selected_output.stdout}{selected_output.stderr}")
                            print(selected_output.stdout.strip())
                        else:
                            selected_output = run(
                                [str(cli), "device", "output", "set",
                                 "skyapo.test.null-target-alt"], env, timeout=8)
                            if ("Playback output switched to "
                                    "skyapo.test.null-target-alt without restarting "
                                    "the audio graph" not in selected_output.stdout):
                                raise RuntimeError(
                                    "CLI did not apply the alternate playback target "
                                    "in-place:\n"
                                    f"{selected_output.stdout}{selected_output.stderr}")
                        deadline = time.monotonic() + 12
                        switched_status = ""
                        switched_node = None
                        while time.monotonic() < deadline:
                            result = run([str(cli), "status"], env, timeout=5,
                                         check=False)
                            switched_status = result.stdout
                            objects = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5).stdout)
                            switched_node = next((
                                str(item.get("id")) for item in objects
                                if item.get("type", "").endswith(":Node") and
                                item.get("info", {}).get("props", item.get("props", {}))
                                .get("node.name") == "skyapo.playback"), None)
                            if (result.returncode == 0 and
                                    "Daemon: streaming" in switched_status and
                                    "Playback target: skyapo.test.null-target-alt (" in
                                    switched_status and
                                    "Playback graph: streaming / streaming, internal links "
                                    "2/2, target links 2/2 channels" in switched_status and
                                    "Active capture links: 2/2" in switched_status and
                                    "Callback allocations: 0" in switched_status and
                                    "Callback deallocations: 0" in switched_status):
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError(
                                "live output switch did not restore both playback "
                                "target links while capture remained active:\n" +
                                switched_status)
                        if switched_node != old_playback_node:
                            raise RuntimeError(
                                "in-place output switching unexpectedly recreated "
                                "the SkyAPO playback node")
                        graph = pipewire_dump_objects(
                            run([str(pw_dump)], env, timeout=5).stdout)
                        nodes, links = {}, set()
                        for item in graph:
                            props = item.get("info", {}).get(
                                "props", item.get("props", {}))
                            if item.get("type", "").endswith(":Node"):
                                if props.get("node.name") in (
                                        "skyapo.test.playback-app",
                                        "skyapo.playback"):
                                    nodes[props["node.name"]] = str(item.get("id"))
                            elif item.get("type", "").endswith(":Link"):
                                links.add((str(props.get("link.output.node", "")),
                                           str(props.get("link.input.node", ""))))
                        if ("skyapo.test.playback-app" not in nodes or
                                "skyapo.playback" not in nodes or
                                (nodes["skyapo.test.playback-app"],
                                 nodes["skyapo.playback"]) not in links):
                            raise RuntimeError(
                                "ordinary playback client detached from the smart "
                                f"filter during target switch: nodes={nodes}, links={links}")
                        # Keep an independent consumer connected to the stable
                        # processed-output node while repeatedly switching its
                        # selected sink. This catches dropouts that a recording
                        # started only after the final switch would miss.
                        transition_consumer_args = consumer_args.copy()
                        transition_consumer_args[
                            transition_consumer_args.index("--duration-ms") + 1] = (
                                "25000")
                        transition_consumer_args[
                            transition_consumer_args.index("--capture-node") + 1] = (
                                "skyapo.playback.dsp")
                        transition_consumer_args += [
                            "--name", "skyapo.test.playback-transition-consumer"]
                        if "--ignore-silent-blocks" in transition_consumer_args:
                            transition_consumer_args.remove("--ignore-silent-blocks")
                        transition_consumer = subprocess.Popen(
                            transition_consumer_args, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

                        # The UI selected the alternate sink above. Exercise
                        # repeated live round trips through the CLI while the
                        # ordinary app producer and independent recorder stay
                        # connected; finish on the alternate sink for the
                        # existing selected-monitor measurement below.
                        rollback_consumer_args = consumer_args.copy()
                        rollback_consumer_args[
                            rollback_consumer_args.index("--duration-ms") + 1] = (
                                "6000")
                        rollback_consumer_args[
                            rollback_consumer_args.index("--capture-node") + 1] = (
                                "skyapo.test.null-target-alt")
                        rollback_consumer_args += [
                            "--name", "skyapo.test.playback-rollback-consumer"]
                        rollback_consumer = subprocess.Popen(
                            rollback_consumer_args, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

                        for switch_index, target in enumerate((
                                "skyapo.test.null-target",
                                "skyapo.test.null-target-alt",
                                "skyapo.test.null-target",
                                "skyapo.test.null-target-alt")):
                            if switch_index == 0:
                                fault_trigger.write_text("fail next stream creation\n")
                            selected = run(
                                [str(cli), "device", "output", "set", target],
                                env, timeout=8, check=False)
                            if switch_index == 0:
                                fault_trigger.unlink(missing_ok=True)
                                if not fault_receipt.exists():
                                    raise RuntimeError(
                                        "playback stream creation failure was not "
                                        "injected; output-switch rollback was not "
                                        "actually exercised")
                                if selected.returncode == 0:
                                    raise RuntimeError(
                                        "injected playback stream replacement failure "
                                        "was unexpectedly reported as success")
                                saved_selection = (config_home / "skyapo" /
                                                   "output-device")
                                if (not saved_selection.exists() or
                                        saved_selection.read_text().splitlines()[0] !=
                                        "skyapo.test.null-target-alt"):
                                    raise RuntimeError(
                                        "failed playback output switch overwrote the "
                                        "previously working output preference")
                                deadline = time.monotonic() + 8
                                rollback_status = ""
                                while time.monotonic() < deadline:
                                    result = run([str(cli), "status"], env,
                                                 timeout=5, check=False)
                                    rollback_status = result.stdout
                                    if (result.returncode == 0 and
                                            "Daemon: streaming" in rollback_status and
                                            "Playback target: "
                                            "skyapo.test.null-target-alt (" in
                                            rollback_status and
                                            "Playback graph: streaming / streaming, "
                                            "internal links 2/2, target links 2/2 "
                                            "channels" in rollback_status and
                                            "Active capture links: 2/2" in
                                            rollback_status):
                                        break
                                    time.sleep(0.05)
                                else:
                                    raise RuntimeError(
                                        "failed output switch did not preserve and "
                                        "restore the previously working PipeWire graph:\n" +
                                        rollback_status + "\n" +
                                        selected.stdout + selected.stderr)
                                continue
                            if (selected.returncode != 0 or
                                    f"Playback output switched to {target} without "
                                    "restarting the audio graph" not in
                                    selected.stdout + selected.stderr):
                                raise RuntimeError(
                                    f"repeated live output switch to {target} failed:\n"
                                    f"{selected.stdout}{selected.stderr}")
                            deadline = time.monotonic() + 12
                            switched_status = ""
                            switched_node = None
                            while time.monotonic() < deadline:
                                result = run([str(cli), "status"], env, timeout=5,
                                             check=False)
                                switched_status = result.stdout
                                objects = pipewire_dump_objects(
                                    run([str(pw_dump)], env, timeout=5).stdout)
                                switched_node = next((
                                    str(item.get("id")) for item in objects
                                    if item.get("type", "").endswith(":Node") and
                                    item.get("info", {}).get(
                                        "props", item.get("props", {}))
                                    .get("node.name") == "skyapo.playback"), None)
                                if (result.returncode == 0 and
                                        f"Playback target: {target} (" in
                                        switched_status and
                                        "Daemon: streaming" in switched_status and
                                        "Playback graph: streaming / streaming, internal links "
                                        "2/2, target links 2/2 channels" in switched_status and
                                        "Active capture links: 2/2" in switched_status and
                                        "Callback allocations: 0" in switched_status and
                                        "Callback deallocations: 0" in switched_status):
                                    break
                                time.sleep(0.1)
                            else:
                                raise RuntimeError(
                                    f"playback did not recover after switching to {target}; "
                                    "capture and callback invariants must remain active:\n"
                                    f"{switched_status}")
                            if (switched_node != old_playback_node or
                                    second_source_process.poll() is not None):
                                raise RuntimeError(
                                    "repeated target switching recreated the playback "
                                    "node or stopped the ordinary app producer")
                            graph = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5).stdout)
                            nodes, links = {}, set()
                            for item in graph:
                                props = item.get("info", {}).get(
                                    "props", item.get("props", {}))
                                if item.get("type", "").endswith(":Node"):
                                    if props.get("node.name") in (
                                            "skyapo.test.playback-app",
                                            "skyapo.playback"):
                                        nodes[props["node.name"]] = str(item.get("id"))
                                elif item.get("type", "").endswith(":Link"):
                                    links.add((str(props.get("link.output.node", "")),
                                               str(props.get("link.input.node", ""))))
                            if ("skyapo.test.playback-app" not in nodes or
                                    "skyapo.playback" not in nodes or
                                    (nodes["skyapo.test.playback-app"],
                                     nodes["skyapo.playback"]) not in links):
                                raise RuntimeError(
                                    "ordinary playback client detached from SkyAPO Playback "
                                    f"after switching to {target}: nodes={nodes}, links={links}")

                        rollback_consumer_output, _ = (
                            rollback_consumer.communicate(timeout=10))
                        rollback_consumer = None
                        rollback_ratio = re.search(
                            r"RMS ratio to expected: ([0-9.]+)",
                            rollback_consumer_output)
                        if (not rollback_ratio or
                                abs(float(rollback_ratio.group(1)) - 1.0) >= 0.03):
                            raise RuntimeError(
                                "independent monitor did not retain processed audio "
                                "on the previous output while a replacement failed:\n" +
                                rollback_consumer_output)
                        print("Injected sink-stream creation failure restored the "
                              "previous target; its independent monitor recorded "
                              f"expected output ratio {float(rollback_ratio.group(1)):.6f}.")

                        transition_output, _ = transition_consumer.communicate(
                            timeout=30)
                        transition_consumer_returncode = (
                            transition_consumer.returncode)
                        transition_consumer = None
                        if transition_consumer_returncode != 0:
                            raise RuntimeError(
                                "independent recorder did not capture continuous processed "
                                "audio across repeated target switches:\n" +
                                transition_output)
                        transition_ratio = re.search(
                            r"RMS ratio to expected: ([0-9.]+)", transition_output)
                        if (not transition_ratio or
                                abs(float(transition_ratio.group(1)) - 1.0) >= 0.03):
                            raise RuntimeError(
                                "processed playback level was not maintained across repeated "
                                "target switches:\n" + transition_output)
                        print(
                            "Independent recorder remained attached to SkyAPO Playback "
                            "during four live sink round trips; measured RMS ratio to "
                            f"expected: {float(transition_ratio.group(1)):.6f}.")

                        switched_consumer_args = consumer_args.copy()
                        switched_consumer_args[
                            switched_consumer_args.index("--duration-ms") + 1] = "5000"
                        switched_consumer_args[
                            switched_consumer_args.index("--capture-node") + 1] = (
                                "skyapo.test.null-target-alt")
                        playback_consumer = subprocess.Popen(
                            switched_consumer_args, env=env, text=True,
                            stdout=playback_consumer_log,
                            stderr=subprocess.STDOUT)
                        playback_consumer.wait(timeout=10)
                        playback_consumer_log.flush()
                        playback_consumer_log.seek(0)
                        switched_result = playback_consumer_log.read()
                        switched_ratio = re.search(
                            r"RMS ratio to expected: ([0-9.]+)", switched_result)
                        if (playback_consumer.returncode != 0 or not switched_ratio or
                                abs(float(switched_ratio.group(1)) - 1.0) >= 0.03):
                            raise RuntimeError(
                                "independent recording after the live target switch "
                                "did not verify the selected output's processed signal:\n"
                                f"{switched_result}\n{switched_status}")
                        print(
                            "skyapo device output set switched the live target "
                            "without restarting either graph; playback node "
                            f"{old_playback_node} remained active, recorded "
                            "monitor RMS ratio to expected: "
                            f"{float(switched_ratio.group(1)):.6f}.")
                else:
                    connect_test_source_to_playback(
                        pw_cli, pw_dump, env, device_name,
                        "skyapo.playback", 2)
                if playback_pdc_recovery:
                    pdc_recovery_config.write_text(rejected_dynamic_pdc)
                    reload_args = [str(cli), "config", "reload"]
                    config_candidate_ready = pathlib.Path(
                        str(config_candidate_pause) + ".ready")
                    config_candidate_release = pathlib.Path(
                        str(config_candidate_pause) + ".release")
                    reload_process = subprocess.Popen(
                        reload_args, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    try:
                        deadline = time.monotonic() + 8
                        while (time.monotonic() < deadline and
                               not config_candidate_ready.exists()):
                            if (reload_process.poll() is not None or
                                    daemon_process.poll() is not None):
                                raise RuntimeError(
                                    "daemon or config reload exited before "
                                    "the PDC candidate barrier")
                            time.sleep(0.01)
                        if not config_candidate_ready.exists():
                            raise RuntimeError(
                                "playback PDC test candidate did not reach "
                                "the deterministic reload barrier")
                        transition_fault_consumer.send_signal(signal.SIGUSR1)
                        transition_fault_arm_output = wait_for_consumer_ready(
                            transition_fault_consumer,
                            "first-fault playback monitor arm",
                            marker="SKYAPO_CONSUMER_MEASUREMENT_ARMED")
                    finally:
                        # Always release the daemon if a monitor/setup check
                        # fails, so teardown never leaves its control loop
                        # blocked at the test-only barrier.
                        config_candidate_release.touch()
                    reload_stdout, _ = reload_process.communicate(timeout=10)
                    reload_result = subprocess.CompletedProcess(
                        reload_args, reload_process.returncode,
                        reload_stdout, "")
                    if (reload_result.returncode != 0 or
                            "Config reload succeeded" not in
                            reload_result.stdout):
                        raise RuntimeError(
                            "unsafe PDC playback candidate could not be "
                            "published for the live-crossfade test:\n"
                            f"{reload_result.stdout}{reload_result.stderr}")
                    deadline = time.monotonic() + 12
                    fault_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        fault_status = result.stdout
                        if (result.returncode == 0 and
                                "Playback DSP: faulted (plugin latency refresh "
                                "failed; output silence until config reload)" in
                                fault_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "playback did not fail closed after its LV2 latency "
                            f"change exceeded the PDC budget:\n{fault_status}")

                    first_fault_audio, _ = transition_fault_consumer.communicate(
                        timeout=12)
                    transition_fault_consumer = None
                    first_fault_audio = (transition_fault_ready_output +
                                         transition_fault_arm_output +
                                         first_fault_audio)
                    first_fault_rms = re.search(r"RMS: ([0-9.]+)",
                                                first_fault_audio)
                    if (not first_fault_rms or
                            float(first_fault_rms.group(1)) >= 1.0e-6):
                        raise RuntimeError(
                            "independent monitor detected audio during the first "
                            "playback quantum that reported unsafe dynamic latency; "
                            f"expected fail-closed silence:\n{first_fault_audio}")
                    print("First-fault monitor capture: " +
                          first_fault_audio.strip())

                    def record_playback(*extra):
                        return run(
                            [str(consumer), *extra, "--expected-rate",
                             str(sample_rate), "--duration-ms", "2000",
                             "--capture-node", "skyapo.test.null-target"],
                            env, timeout=12)

                    silent = record_playback("--expect-silent")
                    silence_rms = re.search(r"RMS: ([0-9.]+)",
                                            silent.stdout)
                    if (not silence_rms or
                            float(silence_rms.group(1)) >= 1.0e-6):
                        raise RuntimeError(
                            "faulted playback graph did not produce measured "
                            f"silence at its monitor:\n{silent.stdout}")

                    boundary_consumer = subprocess.Popen(
                        [str(consumer), "--signal-ready", "--expect-silent",
                         "--expected-rate",
                         str(sample_rate), "--duration-ms", "8000",
                         "--capture-node", "skyapo.test.null-target"],
                        env=env, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
                    boundary_consumer_ready_output = wait_for_consumer_ready(
                        boundary_consumer,
                        "faulted playback boundary monitor")

                    metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                                shutil.which("pw-metadata"))
                    if not metadata:
                        boundary_consumer.kill()
                        boundary_consumer.wait()
                        raise RuntimeError(
                            "pw-metadata is required to test faulted playback "
                            "format rebuild")
                    for key, value in (("clock.force-quantum", "512"),
                                       ("clock.force-rate", str(sample_rate))):
                        run([metadata, "-n", "settings", "0", key, value,
                             "Spa:Int"], env, timeout=5)
                    deadline = time.monotonic() + 12
                    rebuilt_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        rebuilt_status = result.stdout
                        if (result.returncode == 0 and
                                f"{sample_rate} Hz, {channel_count} channels, "
                                "quantum 512" in rebuilt_status and
                                "Playback DSP: faulted (plugin latency refresh "
                                "failed; output silence until config reload)" in
                                rebuilt_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "format-only rebuild cleared the unresolved playback "
                            f"PDC fault or did not rebuild its Engine:\n{rebuilt_status}")
                    time.sleep(0.2)
                    unsafe_reload = run(
                        [str(cli), "config", "reload"], env,
                        timeout=10, check=False)
                    unsafe_reload_audio, _ = boundary_consumer.communicate(
                        timeout=12)
                    unsafe_reload_audio = (boundary_consumer_ready_output +
                                           unsafe_reload_audio)
                    if (unsafe_reload.returncode != 0 or
                            "Config reload succeeded" not in
                            unsafe_reload.stdout + unsafe_reload.stderr):
                        raise RuntimeError(
                            "a plugin whose latency changes only after live "
                            "audio was unexpectedly rejected during silent "
                            f"{unsafe_reload.stdout}{unsafe_reload.stderr}")
                    deadline = time.monotonic() + 8
                    unsafe_reload_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        unsafe_reload_status = result.stdout
                        if (result.returncode == 0 and
                                "Playback DSP: faulted (plugin latency refresh "
                                "failed; output silence until config reload)" in
                                unsafe_reload_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "reloading the same dynamic-latency plugin did not "
                            "fail closed again when live audio revealed its "
                            f"unsafe latency:\n{unsafe_reload_status}")
                    unsafe_reload_rms = re.search(
                        r"RMS: ([0-9.]+)", unsafe_reload_audio)
                    boundary_rms = unsafe_reload_rms
                    if (boundary_consumer.returncode != 0 or
                            not unsafe_reload_rms or
                            float(unsafe_reload_rms.group(1)) >= 1.0e-6):
                        raise RuntimeError(
                            "continuous playback monitor across the quantum "
                            "rebuild and dynamic-latency re-detection detected "
                            "non-silent audio:\n"
                            f"{unsafe_reload_audio}")

                    pdc_recovery_config.write_text("Preamp: -6 dB\n")
                    recovered = run([str(cli), "config", "reload"], env,
                                    timeout=10, check=False)
                    if (recovered.returncode != 0 or
                            "Config reload succeeded" not in recovered.stdout):
                        raise RuntimeError(
                            "validated config could not recover faulted playback:\n"
                            f"{recovered.stdout}{recovered.stderr}")
                    deadline = time.monotonic() + 8
                    recovered_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        recovered_status = result.stdout
                        if (result.returncode == 0 and
                                "Playback DSP: faulted" not in recovered_status and
                                "Playback error: plugin latency refresh failed" not in
                                recovered_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "valid config did not recover playback DSP:\n"
                            f"{recovered_status}")
                    consumer_args[
                        consumer_args.index("--duration-ms") + 1] = "5000"
                    playback_consumer = subprocess.Popen(
                        consumer_args, env=env, text=True,
                        stdout=playback_consumer_log,
                        stderr=subprocess.STDOUT)
                    status = recovered_status
                    print("Playback PDC overflow remained fail-closed through "
                          "a PipeWire quantum rebuild, then a validated config "
                          "reload restored playback; silence RMS "
                          f"{float(boundary_rms.group(1)):.8f}.")
                try:
                    playback_consumer.wait(
                        timeout=35 if wireplumber_playback else 20)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError(
                        "playback test consumer did not finish recording") from error
                playback_consumer_log.flush()
                playback_consumer_log.seek(0)
                playback_result = playback_consumer_log.read()
                ratio_match = re.search(
                    r"RMS ratio to expected: ([0-9.]+)", playback_result)
                maximum_silence_match = re.search(
                    r"Longest consecutive near-silence: \d+ frames "
                    r"\(([0-9.]+) ms\)", playback_result)
                mute_test = mode.endswith("-mute")
                expected_ratio = 0.0 if mute_test else 1.0
                ratio_failed = (not ratio_match or
                                (mute_test and
                                 float(ratio_match.group(1)) >= 1.0e-6) or
                                (not mute_test and
                                 abs(float(ratio_match.group(1)) -
                                     expected_ratio) >= 0.03))
                if playback_consumer.returncode != 0 or ratio_failed:
                    graph = run([str(pw_dump)], env, timeout=5, check=False)
                    graph_summary = (summarize_graph_dump(graph.stdout)
                                     if graph.returncode == 0 else
                                     f"pw-dump failed: {graph.stderr}")
                    raise RuntimeError(
                        "PipeWire playback path failed the numerical DSP/control "
                        f"assertion (exit {playback_consumer.returncode}):\n"
                        f"{playback_result}\nstatus:\n{status}\n"
                        f"PipeWire graph:\n{graph_summary}")
                if playback_isolation:
                    if (not maximum_silence_match or
                            float(maximum_silence_match.group(1)) >= 250.0):
                        raise RuntimeError(
                            "internal playback-link recovery exceeded the "
                            "250 ms consecutive-silence budget or did not "
                            f"report its measurement:\n{playback_result}")
                    print("Longest playback near-silence run across internal "
                          "link recovery: "
                          f"{float(maximum_silence_match.group(1)):.3f} ms "
                          "(budget <250 ms).")
                playback_origin = (
                    "ordinary test playback application"
                    if wireplumber_playback else
                    "deterministic PipeWire test source")
                print("PipeWire playback recording verified: "
                      f"{playback_origin} -> SkyAPO DSP -> selected sink "
                      "monitor -> independent consumer; measured "
                      "output/expected ratio "
                      f"{float(ratio_match.group(1)):.6f}.")
                if mode == "playback-wireplumber":
                    trace_graph_if_requested(
                        "WirePlumber playback capture completed", pw_dump, env)
                if playback_isolation:
                    try:
                        unrelated_playback_consumer.wait(timeout=12)
                    except subprocess.TimeoutExpired as error:
                        raise RuntimeError(
                            "unrelated sink monitor did not finish recording") from error
                    unrelated_playback_log.flush()
                    unrelated_playback_log.seek(0)
                    unrelated_result = unrelated_playback_log.read()
                    unrelated_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)", unrelated_result)
                    if (unrelated_playback_consumer.returncode != 0 or
                            not unrelated_ratio or
                            abs(float(unrelated_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "unrelated playback was not preserved at unity "
                            "gain on its explicitly selected sink "
                            f"(exit {unrelated_playback_consumer.returncode}):\n"
                            f"{unrelated_result}\nstatus:\n{status}")
                    print("Unrelated playback stayed on its separate sink; "
                          "measured output/expected ratio "
                          f"{float(unrelated_ratio.group(1)):.6f} (unity).")
                    wpctl = shutil.which("wpctl")
                    if not wpctl:
                        raise RuntimeError(
                            "wpctl is required to test default-sink changes")
                    graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    alternate_id = next((
                        str(item.get("id")) for item in graph
                        if item.get("type", "").endswith(":Node") and
                        item.get("info", {}).get("props", item.get("props", {}))
                        .get("node.name") == "skyapo.test.null-target-alt"), None)
                    if alternate_id is None:
                        raise RuntimeError(
                            "cannot find the alternate default sink node")
                    run([wpctl, "set-default", alternate_id], env, timeout=5)
                    deadline = time.monotonic() + 5
                    override_status = ""
                    while time.monotonic() < deadline:
                        override_status = run(
                            [str(cli), "status"], env, timeout=5).stdout
                        if ("Playback routing policy: inactive (system default "
                                "was changed externally; SkyAPO left it untouched)"
                                in override_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "runtime did not report the external default-sink "
                            f"change without reclaiming it:\n{override_status}")
                    print("External default-sink change was preserved; SkyAPO "
                          "relinquished route ownership.")
                if playback_pdc_recovery:
                    recovery_status = run(
                        [str(cli), "status"], env, timeout=5).stdout
                    if ("Playback DSP: faulted" in recovery_status or
                            "Callback allocations: 0" not in recovery_status or
                            "Callback deallocations: 0" not in recovery_status):
                        raise RuntimeError(
                            "playback PDC recovery left a fault or violated its "
                            f"realtime allocation invariant:\n{recovery_status}")
                    status = recovery_status
                if playback_pdc_refresh:
                    refreshed_status = run(
                        [str(cli), "status"], env, timeout=5).stdout
                    if ("Playback plugin latency refreshes: 1" not in
                            refreshed_status or
                            "Playback DSP: faulted" in refreshed_status or
                            "Callback allocations: 0" not in refreshed_status or
                            "Callback deallocations: 0" not in refreshed_status):
                        raise RuntimeError(
                            "playback output matched but its own PDC refresh "
                            "or realtime allocation invariants were not "
                            f"verified:\n{refreshed_status}")
                    status = refreshed_status
                    print("Playback Engine independently refreshed the LV2 "
                          "latency/PDC plan; callback allocations and "
                          "deallocations remained zero.")
                if playback_stage:
                    # A config may be valid for capture but invalid for the
                    # playback stage. Reload must reject the pair without
                    # publishing only the capture half of the new config.
                    stage_config.write_text(
                        'If: stage == "capture"\n'
                        "Preamp: -3 dB\n"
                        "Else:\n"
                        "UnsupportedPlaybackStage: true\n"
                        "EndIf:\n")
                    rejected = run([str(cli), "config", "reload"], env,
                                   timeout=10, check=False)
                    if (rejected.returncode == 0 or
                            "playback config reload failed" not in
                            rejected.stdout + rejected.stderr):
                        raise RuntimeError(
                            "stage-incompatible config was not rejected as an "
                            "atomic capture/playback reload:\n"
                            f"{rejected.stdout}{rejected.stderr}")
                    rejected_status = run(
                        [str(cli), "status"], env, timeout=5).stdout
                    rejected_capture_ratio = re.search(
                        r"DSP amplitude ratio: ([0-9.]+)", rejected_status)
                    if (not rejected_capture_ratio or
                            abs(float(rejected_capture_ratio.group(1)) - 1.0) >=
                            0.01 or
                            "Config reload error:" not in rejected_status):
                        raise RuntimeError(
                            "failed playback validation partially changed the "
                            f"capture engine:\n{rejected_status}")
                    verify_last_good = run(
                        [str(consumer), "--expected-rate", str(sample_rate),
                         "--expected-db", "-6", "--duration-ms", "5000",
                         "--capture-node", "skyapo.test.null-target",
                         "--ignore-silent-blocks", "--internal-filter"],
                        env, timeout=12)
                    retained_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)",
                        verify_last_good.stdout)
                    if (not retained_ratio or
                            abs(float(retained_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "failed playback validation did not retain the "
                            "last accepted playback DSP:\n"
                            f"{verify_last_good.stdout}\n{rejected_status}")

                    metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                                shutil.which("pw-metadata"))
                    if not metadata:
                        raise RuntimeError(
                            "pw-metadata is required for last-good format rebuild")
                    for key, value in (("clock.force-quantum", "512"),
                                       ("clock.force-rate", "44100")):
                        run([metadata, "-n", "settings", "0", key, value,
                             "Spa:Int"], env, timeout=5)
                    deadline = time.monotonic() + 12
                    reconfigured_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        reconfigured_status = result.stdout
                        if (result.returncode == 0 and
                                "Sample rate: 44100 Hz" in reconfigured_status and
                                "Quantum: 512" in reconfigured_status and
                                "Playback format: 44100 Hz, 2 channels, quantum "
                                "512" in reconfigured_status and
                                "Active capture links: 2/2" in reconfigured_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "last-good config did not rebuild both graphs after "
                            "a format change while the on-disk playback branch "
                            f"was invalid:\n{reconfigured_status}")
                    # The remaining shared capture checks use this negotiated
                    # rate; the test graph now runs at 44.1 kHz, not its initial
                    # 48 kHz rate.
                    sample_rate = 44100
                    reconfigured_capture_ratio = re.search(
                        r"DSP amplitude ratio: ([0-9.]+)",
                        reconfigured_status)
                    if (not reconfigured_capture_ratio or
                            abs(float(reconfigured_capture_ratio.group(1)) - 1.0)
                            >= 0.01):
                        raise RuntimeError(
                            "capture format rebuild did not retain its last-good "
                            f"config:\n{reconfigured_status}")
                    verify_reconfigured = run(
                        [str(consumer), "--expected-rate", "44100",
                         "--expected-db", "-6", "--duration-ms", "5000",
                         "--capture-node", "skyapo.test.null-target",
                         "--ignore-silent-blocks", "--internal-filter"],
                        env, timeout=12)
                    reconfigured_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)",
                        verify_reconfigured.stdout)
                    if (not reconfigured_ratio or
                            abs(float(reconfigured_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "playback last-good snapshot was not used after the "
                            "PipeWire format change:\n"
                            f"{verify_reconfigured.stdout}\n"
                            f"{reconfigured_status}")

                    stage_config.write_text(
                        'If: stage == "post-mix"\n'
                        "Preamp: -3 dB\n"
                        "Else:\n"
                        "Preamp: 0 dB\n"
                        "EndIf:\n")
                    recovered = run([str(cli), "config", "reload"], env,
                                    timeout=10, check=False)
                    if (recovered.returncode != 0 or
                            "Config reload succeeded" not in recovered.stdout):
                        raise RuntimeError(
                            "valid stage-specific config did not recover both "
                            f"processing graphs:\n{recovered.stdout}")
                    verify_recovered = run(
                        [str(consumer), "--expected-rate", "44100",
                         "--expected-db", "-3", "--duration-ms", "5000",
                         "--capture-node", "skyapo.test.null-target",
                         "--ignore-silent-blocks", "--internal-filter"],
                        env, timeout=12)
                    recovered_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)",
                        verify_recovered.stdout)
                    if (not recovered_ratio or
                            abs(float(recovered_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "valid stage-specific reload did not update the "
                            "playback DSP:\n" f"{verify_recovered.stdout}")
                    recovered_status = run(
                        [str(cli), "status"], env, timeout=5).stdout
                    recovered_capture_ratio = re.search(
                        r"DSP amplitude ratio: ([0-9.]+)", recovered_status)
                    if (not recovered_capture_ratio or
                            abs(float(recovered_capture_ratio.group(1)) - 1.0) >=
                            0.01 or
                            "Config reload error:" in recovered_status):
                        raise RuntimeError(
                            "valid stage-specific reload did not update capture "
                            f"state cleanly:\n{recovered_status}")
                    print("A playback-invalid stage config was rejected without "
                          "changing either graph; valid stage reload recovered "
                          "both capture and playback DSP.")
                daemon_alive_status = run(
                    [str(cli), "status"], env, timeout=5, check=False)
                if (daemon_process.poll() is not None or
                        daemon_alive_status.returncode != 0 or
                        "Daemon: streaming" not in daemon_alive_status.stdout or
                        f"Active capture links: {channel_count}/{channel_count}"
                        not in daemon_alive_status.stdout):
                    raise RuntimeError(
                        "removing the temporary playback endpoint stopped or "
                        "damaged the capture graph:\n" +
                        daemon_alive_status.stdout)
                status = daemon_alive_status.stdout

            if plugin_process_failure and not vst3_exception_failure:
                deadline = time.monotonic() + 8
                failure_status = ""
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited after CLAP reported a process error")
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    failure_status = result.stdout
                    plugin_failure_visible = (
                        "Plugin latency update error: unknown non-standard "
                        "exception during plugin latency refresh" in
                        failure_status if vst3_nonstd_latency_failure else
                        failing_plugin_id in failure_status)
                    if (result.returncode == 0 and
                            "Daemon: streaming" in failure_status and
                            plugin_failure_visible):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "CLAP process failure was not reported while the "
                        f"daemon remained streaming:\n{failure_status}")
                status = failure_status

            if lv2_pdc_config_commit_race:
                # Connect the independent recorder before the control thread
                # is paused at the config-commit barrier. The virtual-source
                # format handshake needs that thread to remain responsive.
                race_monitor = subprocess.Popen(
                    [str(consumer), "--expect-silent", "--expected-rate",
                     str(sample_rate), "--duration-ms", "10000",
                     "--signal-connected", "--arm-on-signal", "--name",
                     "skyapo.test.pdc-config-commit-monitor"],
                    env=env, text=True, stdout=race_monitor_log,
                    stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 8
                monitor_log = ""
                while time.monotonic() < deadline:
                    if race_monitor.poll() is not None:
                        race_monitor_log.flush()
                        race_monitor_log.seek(0)
                        monitor_log = race_monitor_log.read()
                        raise RuntimeError(
                            "continuous PDC monitor exited before connecting "
                            f"to the virtual microphone:\n{monitor_log}")
                    race_monitor_log.flush()
                    race_monitor_log.seek(0)
                    monitor_log = race_monitor_log.read()
                    if "SKYAPO_CONSUMER_CONNECTED" in monitor_log:
                        break
                    time.sleep(0.01)
                else:
                    raise RuntimeError(
                        "continuous PDC monitor did not connect before the "
                        f"config barrier:\n{monitor_log}")

                pdc_recovery_config.write_text("Preamp: -6 dB\n")
                config_reload_process = subprocess.Popen(
                    [str(cli), "config", "reload"],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                ready_marker = pathlib.Path(
                    str(config_candidate_pause) + ".ready")
                release_marker = pathlib.Path(
                    str(config_candidate_pause) + ".release")
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and not ready_marker.exists():
                    if config_reload_process.poll() is not None:
                        stdout, stderr = config_reload_process.communicate()
                        raise RuntimeError(
                            "config candidate-build race hook was bypassed; "
                            f"reload exited early:\n{stdout}{stderr}")
                    time.sleep(0.01)
                if not ready_marker.exists():
                    config_reload_process.terminate()
                    stdout, stderr = config_reload_process.communicate(timeout=5)
                    raise RuntimeError(
                        "daemon did not reach the post-candidate/pre-commit "
                        f"barrier:\n{stdout}{stderr}")

                # The candidate is ready but not yet published; the independent
                # recorder already has an active route to the virtual mic.
                try:
                    # First silence the right-channel-only warm-up signal, then
                    # arm the recorder's numerical window. SIGUSR1 starts both
                    # channels; the LV2 plugin on FL then reports excessive
                    # latency while the control thread remains at the barrier.
                    source_process.send_signal(signal.SIGUSR2)
                    time.sleep(0.05)
                    race_monitor.send_signal(signal.SIGUSR1)
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        if race_monitor.poll() is not None:
                            raise RuntimeError(
                                "continuous PDC monitor exited while arming "
                                f"silence measurement (status "
                                f"{race_monitor.returncode})")
                        race_monitor_log.flush()
                        race_monitor_log.seek(0)
                        monitor_log = race_monitor_log.read()
                        if "SKYAPO_CONSUMER_MEASUREMENT_ARMED" in monitor_log:
                            break
                        time.sleep(0.01)
                    else:
                        raise RuntimeError(
                            "continuous PDC monitor did not arm its silence "
                            f"measurement:\n{monitor_log}")
                    source_process.send_signal(signal.SIGUSR1)
                    race_monitor.wait(timeout=3)
                    if source_process.poll() is not None:
                        source_log.flush()
                        source_log.seek(0)
                        raise RuntimeError(
                            "the selected PipeWire test source exited during "
                            "the PDC config-commit race "
                            f"(status {source_process.returncode}):\n"
                            f"{source_log.read()}")
                finally:
                    release_marker.touch()
                race_monitor_log.flush()
                race_monitor_log.seek(0)
                silent_output = race_monitor_log.read()
                silence_rms = re.search(r"RMS: ([0-9.]+)", silent_output)
                if (not silence_rms or
                        float(silence_rms.group(1)) >= 1.0e-6 or
                        race_monitor.returncode != 0):
                    config_reload_process.communicate(timeout=8)
                    raise RuntimeError(
                        "the active plugin did not fail closed after its live "
                        "latency increased during candidate construction:\n"
                        f"{silent_output}")

                reload_stdout, reload_stderr = config_reload_process.communicate(
                    timeout=10)
                if (config_reload_process.returncode != 0 or
                        "Config reload succeeded" not in
                        reload_stdout + reload_stderr):
                    raise RuntimeError(
                        "validated config commit did not complete after the "
                        "late PDC event:\n"
                        f"{reload_stdout}{reload_stderr}")

                deadline = time.monotonic() + 8
                recovered_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    recovered_status = result.stdout
                    if (result.returncode == 0 and
                            "DSP state: running" in recovered_status and
                            "Plugin latency update error:" not in
                            recovered_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "candidate built before the PDC event was not safely "
                        "validated and committed:\n" + recovered_status)
                # The file write also schedules the automatic inotify reload.
                # The explicit CLI reload above must consume that pending
                # debounce instead of starting a duplicate graph transition.
                time.sleep(0.25)
                recovered_status = run(
                    [str(cli), "status"], env, timeout=5).stdout
                if ("Config reload error:" in recovered_status or
                        "Daemon: streaming" not in recovered_status):
                    raise RuntimeError(
                        "explicit config reload left a duplicate watcher "
                        "reload or damaged the recovered daemon:\n" +
                        recovered_status)
                recovered_audio = run(
                    [str(consumer), "--expected-rate", str(sample_rate),
                     "--expected-db", "-6"], env, timeout=10)
                recovered_ratio = re.search(
                    r"RMS ratio to expected: ([0-9.]+)",
                    recovered_audio.stdout)
                if (not recovered_ratio or
                        abs(float(recovered_ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        "late-PDC recovery did not produce the replacement "
                        f"config's measured -6 dB signal:\n"
                        f"{recovered_audio.stdout}")
                status = recovered_status
                print(
                    "A live LV2 latency fault arriving after candidate build "
                    "kept the virtual microphone silent until the validated "
                    "replacement committed, then recovered to measured -6 "
                    f"dB (silence RMS {float(silence_rms.group(1)):.8f}, "
                    f"ratio {float(recovered_ratio.group(1)):.6f}).")
                race_monitor = None

            if pdc_format_event_order:
                race_monitor = subprocess.Popen(
                    [str(consumer), "--expect-silent", "--expected-rate",
                     str(sample_rate), "--duration-ms", "2500",
                     "--signal-connected", "--name",
                     "skyapo.test.pdc-format-event-monitor"],
                    env=env, text=True, stdout=race_monitor_log,
                    stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 8
                monitor_log = ""
                while time.monotonic() < deadline:
                    if race_monitor.poll() is not None:
                        race_monitor_log.flush()
                        race_monitor_log.seek(0)
                        monitor_log = race_monitor_log.read()
                        raise RuntimeError(
                            "PDC format-order monitor exited before becoming "
                            f"ready:\n{monitor_log}")
                    race_monitor_log.flush()
                    race_monitor_log.seek(0)
                    monitor_log = race_monitor_log.read()
                    if "SKYAPO_CONSUMER_CONNECTED" in monitor_log:
                        break
                    time.sleep(0.01)
                else:
                    raise RuntimeError(
                        "PDC format-order monitor did not connect to the "
                        f"virtual microphone:\n{monitor_log}")

                # This signal makes the fixture raise its live latency. The
                # daemon's test-only control hook invokes a 2048-frame format
                # rebuild while the PDC event is still queued, reproducing the
                # control-event ordering without changing the PipeWire server.
                source_process.send_signal(signal.SIGUSR1)
                race_monitor.wait(timeout=8)
                if source_process.poll() is not None:
                    source_log.flush()
                    source_log.seek(0)
                    raise RuntimeError(
                        "the selected PipeWire test source exited during "
                        "the PDC format-event-order race "
                        f"(status {source_process.returncode}):\n"
                        f"{source_log.read()}")
                race_monitor_log.flush()
                race_monitor_log.seek(0)
                silent_output = race_monitor_log.read()
                silence_rms = re.search(r"RMS: ([0-9.]+)", silent_output)
                if (race_monitor.returncode != 0 or not silence_rms or
                        float(silence_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "format rebuild ordered ahead of the queued PDC "
                        "event allowed audio to resume before validation:\n"
                        f"{silent_output}")
                deferred_status = run([str(cli), "status"], env, timeout=5,
                                      check=False)
                if (deferred_status.returncode != 0 or
                        "Test PDC format rebuild: deferred" not in
                        deferred_status.stdout):
                    raise RuntimeError(
                        "the queued-PDC test did not enter the guarded "
                        "format-rebuild branch:\n"
                        f"{deferred_status.stdout}{deferred_status.stderr}")
                race_monitor = None

            if lv2_pdc_recovery and not playback_pdc_recovery:
                deadline = time.monotonic() + 8
                fault_status = ""
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited instead of preserving its control "
                            "socket after the rejected PDC update")
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    fault_status = result.stdout
                    if (result.returncode == 0 and
                            "DSP state: paused (plugin latency refresh failed; "
                            "output silence)" in fault_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "dynamic PDC budget failure did not leave the daemon "
                        f"in its recoverable silence state:\n{fault_status}")
                if (mode == "lv2-pdc-recovery" and
                        "Test latency callback drain timeout: injected" not in
                        fault_status):
                    raise RuntimeError(
                        "PDC recovery test did not exercise the callback-drain "
                        f"timeout path:\n{fault_status}")
                fault_node = virtual_source_id(fault_status)
                assert_virtual_source(pw_dump, env, fault_node)
                silent = run([str(consumer), "--expect-silent",
                              "--expected-rate", str(sample_rate)],
                             env, timeout=12)
                silence_rms = re.search(r"RMS: ([0-9.]+)", silent.stdout)
                if (not silence_rms or float(silence_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "recoverable PDC fault did not produce measured "
                        f"virtual-microphone silence:\n{silent.stdout}")

                unsafe_capture_reload_log_path = (
                    root / "unsafe-capture-reload-consumer.log")
                with unsafe_capture_reload_log_path.open("w+") as log:
                    unsafe_capture_consumer = subprocess.Popen(
                        [str(consumer), "--expect-silent", "--expected-rate",
                         str(sample_rate), "--duration-ms", "3000"],
                        env=env, text=True, stdout=log,
                        stderr=subprocess.STDOUT)
                    time.sleep(0.2)
                    unsafe_capture_reload = run(
                        [str(cli), "config", "reload"], env, timeout=10,
                        check=False)
                    unsafe_capture_consumer.wait(timeout=12)
                    log.flush()
                    log.seek(0)
                    unsafe_capture_audio = log.read()
                if (unsafe_capture_reload.returncode != 0 or
                        "Config reload succeeded" not in
                        unsafe_capture_reload.stdout +
                        unsafe_capture_reload.stderr):
                    raise RuntimeError(
                        "a plugin whose latency changes only after live audio "
                        "was unexpectedly rejected during silent capture "
                        f"{unsafe_capture_reload.stdout}"
                        f"{unsafe_capture_reload.stderr}")
                deadline = time.monotonic() + 8
                unsafe_capture_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    unsafe_capture_status = result.stdout
                    if (result.returncode == 0 and
                            "DSP state: paused (plugin latency refresh failed; "
                            "output silence)" in unsafe_capture_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "reloading the same dynamic-latency capture plugin did "
                        "not fail closed again when live audio revealed its "
                        f"unsafe latency:\n{unsafe_capture_status}")
                unsafe_capture_rms = re.search(
                    r"RMS: ([0-9.]+)", unsafe_capture_audio)
                if (unsafe_capture_consumer.returncode != 0 or
                        not unsafe_capture_rms or
                        float(unsafe_capture_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "continuous capture monitor detected non-silent audio "
                        "across dynamic-latency re-detection after config "
                        "reload:\n"
                        f"{unsafe_capture_audio}")

                pdc_recovery_config.write_text("UnsupportedPdcRecovery: true\n")
                rejected = run([str(cli), "config", "reload"], env,
                               timeout=10, check=False)
                if (rejected.returncode == 0 or
                        "Config reload failed" not in
                        rejected.stdout + rejected.stderr):
                    raise RuntimeError(
                        "invalid config was not rejected while recovering "
                        "from PDC failure:\n"
                        f"{rejected.stdout}{rejected.stderr}")
                still_faulted = run([str(cli), "status"], env,
                                    timeout=5, check=False)
                if (still_faulted.returncode != 0 or
                        "DSP state: paused (plugin latency refresh failed; "
                        "output silence)" not in still_faulted.stdout):
                    raise RuntimeError(
                        "invalid config incorrectly resumed the faulted "
                        f"engine:\n{still_faulted.stdout}")

                metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                            shutil.which("pw-metadata"))
                if not metadata:
                    raise RuntimeError(
                        "pw-metadata is required to test faulted format rebuild")
                for key, value in (("clock.force-quantum", "512"),
                                   ("clock.force-rate", str(sample_rate))):
                    run([metadata, "-n", "settings", "0", key, value,
                         "Spa:Int"], env, timeout=5)
                deadline = time.monotonic() + 12
                rebuilt_fault_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    rebuilt_fault_status = result.stdout
                    if (result.returncode == 0 and
                            f"Quantum: 512" in rebuilt_fault_status and
                            "DSP state: paused (plugin latency refresh failed; "
                            "output silence)" in rebuilt_fault_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "PipeWire quantum rebuild resumed or lost the faulted "
                        f"DSP state:\n{rebuilt_fault_status}")
                still_silent = run([str(consumer), "--expect-silent",
                                    "--expected-rate", str(sample_rate)],
                                   env, timeout=12)
                silence_rms = re.search(r"RMS: ([0-9.]+)",
                                        still_silent.stdout)
                if (not silence_rms or
                        float(silence_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "faulted format rebuild resumed audio before a valid "
                        f"config reload:\n{still_silent.stdout}")

                pdc_recovery_config.write_text("Preamp: -6 dB\n")
                recovered = run([str(cli), "config", "reload"], env,
                                timeout=10, check=False)
                if (recovered.returncode != 0 or
                        "Config reload succeeded" not in recovered.stdout):
                    raise RuntimeError(
                        "valid config could not recover the paused DSP graph:\n"
                        f"{recovered.stdout}")
                deadline = time.monotonic() + 8
                recovered_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    recovered_status = result.stdout
                    if (result.returncode == 0 and
                            "DSP state: running" in recovered_status and
                            "Plugin latency update error:" not in recovered_status):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "valid config reload did not resume DSP after the PDC "
                        f"fault:\n{recovered_status}")
                recovered_audio = run(
                    [str(consumer), "--expected-rate", str(sample_rate),
                     "--expected-db", "-6"], env, timeout=12)
                recovered_ratio = re.search(
                    r"RMS ratio to expected: ([0-9.]+)", recovered_audio.stdout)
                if (not recovered_ratio or
                        abs(float(recovered_ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        "recovered virtual microphone did not carry expected "
                        f"-6 dB audio:\n{recovered_audio.stdout}")
                status = recovered_status
                print("Dynamic PDC budget failure kept skyapod and its virtual "
                      "source alive, emitted measured silence, then a valid "
                      "config hard-recovered to measured -6 dB audio; "
                      f"silence RMS {float(silence_rms.group(1)):.8f}, "
                      f"recovery ratio {float(recovered_ratio.group(1)):.6f}.")

            if systemd_crash_restart:
                old_pid = daemon_process.main_pid()
                restart_count = daemon_process.restart_count()
                if old_pid <= 0:
                    raise RuntimeError(
                        "systemd did not publish the supervised skyapod PID")
                if playback_systemd_crash_restart:
                    stop(playback_consumer,
                         "pre-crash playback monitor consumer")
                    playback_consumer = None
                    stop(second_source_process,
                         "pre-crash ordinary playback stream")
                    second_source_process = None
                daemon_process.send_signal(signal.SIGKILL)
                if playback_systemd_user_override:
                    # Hold systemd's automatic restart long enough to model a
                    # user changing the default while the daemon is down.
                    override_graph = pipewire_dump_objects(
                        run([str(pw_dump)], env, timeout=5).stdout)
                    override_target_id = next((
                        str(item.get("id")) for item in override_graph
                        if item.get("type", "").endswith(":Node") and
                        item.get("info", {}).get(
                            "props", item.get("props", {})).get("node.name") ==
                        "skyapo.test.null-target-alt"), None)
                    if override_target_id is None:
                        raise RuntimeError(
                            "cannot find alternate target for simulated user "
                            "default change")
                    run([wpctl, "set-default", override_target_id], env,
                        timeout=5)
                deadline = time.monotonic() + 15
                restarted_status = ""
                new_pid = 0
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "systemd left the daemon stopped instead of restarting it")
                    new_pid = daemon_process.main_pid()
                    restarts = daemon_process.restart_count()
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    restarted_status = result.stdout
                    if (new_pid > 0 and new_pid != old_pid and
                            (playback_systemd_user_override or
                             restarts > restart_count) and result.returncode == 0 and
                            "Daemon: streaming" in restarted_status and
                            "Active capture links: " +
                            f"{channel_count}/{channel_count}" in
                            restarted_status and
                            (not playback_systemd_crash_restart or
                             playback_systemd_user_override or
                             "Playback graph: streaming / streaming, "
                             "internal links 2/2, target links 2/2 channels"
                             in restarted_status)):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "systemd did not restart skyapod after SIGKILL with a "
                        f"streaming graph:\n{restarted_status}\n"
                        f"--- {daemon_process.unit} journal ---\n"
                        f"{daemon_process.journal()}")
                status = restarted_status
                new_virtual_id = virtual_source_id(status)
                assert_virtual_source(pw_dump, env, new_virtual_id)
                if playback_systemd_user_override:
                    override_deadline = time.monotonic() + 8
                    while time.monotonic() < override_deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        restarted_status = result.stdout
                        if (result.returncode == 0 and
                                "system default was changed externally; "
                                "SkyAPO left it untouched" in restarted_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "daemon did not report preserving the playback "
                            "default changed while it was stopped:\n" +
                            restarted_status)
                if playback_systemd_crash_restart and not playback_systemd_user_override:
                    recovery_deadline = time.monotonic() + 5
                    restart_journal = ""
                    while time.monotonic() < recovery_deadline:
                        restart_journal = daemon_process.journal()
                        if ("recovered playback route ownership from the "
                                "saved pre-crash default" in restart_journal):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "systemd restarted playback without reclaiming the "
                            "persisted pre-crash default route:\n"
                            f"{restart_journal}")
                print(f"User-systemd crash recovery passed: skyapod PID "
                      f"{old_pid} -> {new_pid}; restart count "
                      f"{daemon_process.restart_count()}; virtual source "
                      f"node {old_virtual_id} -> {new_virtual_id}.")
                if playback_systemd_crash_restart and not playback_systemd_user_override:
                    print("Restarted daemon reclaimed the persisted original "
                          "playback route lease.")
                if (playback_systemd_crash_restart and
                        not playback_systemd_user_override):
                    second_source_process = subprocess.Popen(
                        [str(playback_client)], env=env, text=True,
                        stdout=source_log, stderr=subprocess.STDOUT)
                    wait_for_playback_application_route(
                        second_source_process, pw_dump, env, channel_count)
                    pre_dsp_playback = run(
                        [str(consumer), "--expected-db", "0",
                         "--expected-rate", str(sample_rate),
                         "--duration-ms", "1500", "--capture-node",
                         "skyapo.playback", "--signal-ready",
                         "--signal-connected", "--internal-filter", "--name",
                         "skyapo.test.consumer.playback-input-after-restart"],
                        env, timeout=8, check=False)
                    pre_dsp_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)",
                        pre_dsp_playback.stdout)
                    if (pre_dsp_playback.returncode != 0 or
                            not pre_dsp_ratio or
                            abs(float(pre_dsp_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "systemd-restarted PipeWire playback input monitor "
                            "did not receive the expected unity-level test "
                            f"signal:\n{pre_dsp_playback.stdout}")
                    print("Post-restart pre-DSP playback input was measured "
                          "at unity; output/expected RMS ratio "
                          f"{float(pre_dsp_ratio.group(1)):.6f}.")
                    post_crash_playback_consumer = subprocess.Popen(
                        [str(consumer), "--expected-db", "-6",
                         "--expected-rate", str(sample_rate),
                         "--duration-ms", "2500", "--capture-node",
                         "skyapo.test.null-target", "--signal-ready",
                         "--signal-connected",
                         "--name", "skyapo.test.consumer.after-daemon-restart"],
                        env=env, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
                    ready_output = wait_for_consumer_ready(
                        post_crash_playback_consumer,
                        "post-crash independent playback monitor")
                    try:
                        remaining_output, _ = (
                            post_crash_playback_consumer.communicate(
                                timeout=10))
                    except subprocess.TimeoutExpired as error:
                        raise RuntimeError(
                            "post-crash playback monitor did not finish") from error
                    playback_output = ready_output + remaining_output
                    playback_ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)", playback_output)
                    if (post_crash_playback_consumer.returncode != 0 or
                            not playback_ratio or
                            abs(float(playback_ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "systemd-restarted daemon did not process ordinary "
                            "playback to the expected -6 dB sink-monitor level:\n"
                            f"{playback_output}\n{restarted_status}")
                    stop(second_source_process,
                         "post-crash ordinary playback stream")
                    second_source_process = None
                    post_crash_playback_consumer = None
                    print("Post-crash ordinary playback stream was measured "
                          "at the independent sink monitor; output/expected "
                          f"RMS ratio {float(playback_ratio.group(1)):.6f}.")

            if device_switch:
                second_name = "skyapo.test.alternate"
                second_source_process = subprocess.Popen(
                    [str(source), "--name", second_name], env=env,
                    stdout=source_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if second_source_process.poll() is not None:
                        raise RuntimeError("alternate test source exited")
                    devices = run([str(cli), "device", "list"], env)
                    if second_name in devices.stdout:
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "alternate capture source was not enumerated")
                selected = run([str(cli), "device", "set", second_name], env)
                if second_name not in selected.stdout:
                    raise RuntimeError(
                        f"live device switch was not persisted: {selected.stdout}")
                deadline = time.monotonic() + 20
                switched_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, check=False)
                    switched_status = result.stdout
                    switched_capture = re.search(
                        r"^Capture node: (\d+)", switched_status, re.MULTILINE)
                    if (result.returncode == 0 and
                            "Daemon: streaming" in switched_status and
                            f"Selected device: {second_name}" in
                            switched_status and
                            switched_capture and
                            switched_capture.group(1) !=
                            old_capture_id.group(1) and
                            f"Active capture links: {channel_count}/{channel_count}"
                            in switched_status):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "daemon did not rebuild the live graph on the newly "
                        f"selected stable node name:\n{switched_status}")
                status = switched_status
                assert_virtual_source(pw_dump, env, virtual_source_id(status))
                print("Live device selection rebuilt capture from "
                      f"{device_name} to {second_name}; capture node "
                      f"{old_capture_id.group(1)} -> {switched_capture.group(1)}, "
                      "with the virtual source streaming.")

            if transition_format:
                transition_config.write_text("Preamp: -3 dB\n")
                reload_result = run(
                    [str(cli), "config", "reload"], env, timeout=8)
                if "Config reload succeeded" not in reload_result.stdout:
                    raise RuntimeError(
                        "transition test config did not reload:\n"
                        f"{reload_result.stdout}")
                deadline = time.monotonic() + 5
                transition_status = ""
                while time.monotonic() < deadline:
                    transition_status = run([str(cli), "status"], env).stdout
                    if "Graph transition: crossfading" in transition_status:
                        break
                    time.sleep(0.02)
                else:
                    raise RuntimeError(
                        "long test transition was not published:\n"
                        f"{transition_status}")

                metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                            shutil.which("pw-metadata"))
                if not metadata:
                    raise RuntimeError("pw-metadata is required for transition test")
                run([metadata, "-n", "settings", "0", "clock.force-rate",
                     "44100", "Spa:Int"], env, timeout=5)
                deadline = time.monotonic() + 12
                rebuilt_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, check=False)
                    rebuilt_status = result.stdout
                    if (result.returncode == 0 and
                            "Sample rate: 44100 Hz" in rebuilt_status and
                            "Quantum: 512" in rebuilt_status and
                            "Graph transition: stable" in rebuilt_status and
                            "Format rebuilds during transition: 1" in
                            rebuilt_status and
                            "Active capture links: " +
                            f"{channel_count}/{channel_count}" in
                            rebuilt_status):
                        break
                    time.sleep(0.02)
                else:
                    raise RuntimeError(
                        "format change did not rebuild while the graph "
                        "transition was pending:\n"
                        f"{rebuilt_status}")
                status = rebuilt_status
                sample_rate = 44100
                capture = run(
                    [str(consumer), "--expected-rate", str(sample_rate),
                     "--expected-db", "-3"], env, timeout=12)
                ratio = re.search(r"RMS ratio to expected: ([0-9.]+)",
                                  capture.stdout)
                if (not ratio or
                        abs(float(ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        "format change during crossfade did not preserve the "
                        f"accepted DSP graph:\n{capture.stdout}")
                print("Sample-rate change arrived during a published 500 ms "
                      "test crossfade; Runtime confirmed the pending-format "
                      "rebuild branch, then recorded the -3 dB graph at "
                      f"44.1 kHz (ratio {float(ratio.group(1)):.6f}).")

            if renegotiate:
                metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                            shutil.which("pw-metadata"))
                if not metadata:
                    raise RuntimeError("pw-metadata is required for renegotiate E2E")

                def force_clock(rate, quantum):
                    for key, value in (("clock.force-quantum", quantum),
                                       ("clock.force-rate", rate)):
                        run([metadata, "-n", "settings", "0", key,
                             str(value), "Spa:Int"], env, timeout=5)
                    deadline = time.monotonic() + 12
                    current_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        current_status = result.stdout
                        if (result.returncode == 0 and
                                f"Sample rate: {rate} Hz" in current_status and
                                f"Quantum: {quantum}" in current_status and
                                "Active capture links: " +
                                f"{channel_count}/{channel_count}" in
                                current_status):
                            return current_status
                        time.sleep(0.05)
                    raise RuntimeError(
                        f"runtime format did not renegotiate to {rate} Hz / "
                        f"{quantum}:\n{current_status}")

                for new_rate, new_quantum in ((48000, 512),
                                              (44100, 512), (96000, 2048)):
                    changed_status = force_clock(new_rate, new_quantum)
                    status = changed_status
                    output = run(
                        [str(consumer), "--expected-rate", str(new_rate),
                         "--expected-db", "-6"], env, timeout=12)
                    ratio = re.search(r"RMS ratio to expected: ([0-9.]+)",
                                      output.stdout)
                    if (not ratio or
                            abs(float(ratio.group(1)) - 1.0) >= 0.03):
                        raise RuntimeError(
                            "format renegotiation did not preserve processed "
                            f"audio at {new_rate}/{new_quantum}:\n"
                            f"{changed_status}\n{output.stdout}")
                    print(f"Live graph reconfigured to {new_rate} Hz / "
                          f"{new_quantum}; independent capture ratio "
                          f"{float(ratio.group(1)):.6f}.")
                    sample_rate = new_rate

            if lv2_pdc_final or mode in (
                    "playback-preamp", "playback-wireplumber",
                    "playback-wireplumber-volume",
                    "playback-wireplumber-stage",
                    "playback-wireplumber-mute"):
                # Keep the input graph steady while measuring channel phase:
                # these cases verify their dedicated playback/PDC processing;
                # link-removal recovery is covered by dedicated E2E cases.
                link_id = "not removed (dedicated processing measurement)"
            else:
                link_id = destroy_capture_link(pw_cli, pw_dump, env, status)
                deadline = time.monotonic() + 8
                recovered_status = ""
                expected_links = f"Active capture links: {channel_count}/{channel_count}"
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError("skyapod exited after capture-link removal")
                    result = run([str(cli), "status"], env, timeout=5, check=False)
                    recovered_status = result.stdout
                    if result.returncode == 0 and expected_links in recovered_status:
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        f"capture link {link_id} was not restored:\n{recovered_status}")
                status = recovered_status

            if source_replug or server_restart:
                if playback_server_restart:
                    stop(playback_consumer,
                         "pre-restart playback monitor consumer")
                    playback_consumer = None
                    stop(second_source_process,
                         "pre-restart ordinary playback stream")
                    second_source_process = None
                    stop(wireplumber_process,
                         "pre-restart WirePlumber policy")
                    wireplumber_process = None
                    stop(null_target_manager,
                         "pre-restart playback target manager")
                    null_target_manager = None
                print("Stopping selected deterministic source…", flush=True)
                daemon_log_offset = os.fstat(daemon_log.fileno()).st_size
                stop(source_process, "deterministic capture source")
                source_process = None
                deadline = time.monotonic() + 8
                became_unavailable = False
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited after selected source removal")
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    if (result.returncode != 0 or
                            "Daemon: not reachable" in result.stdout):
                        became_unavailable = True
                        break
                    time.sleep(0.1)
                if not became_unavailable:
                    raise RuntimeError(
                        "daemon status socket remained available after its "
                        "selected source disappeared")
                if server_restart:
                    # Socket disappearance means Runtime teardown has begun,
                    # not that its destructor has finished draining pending
                    # PipeWire metadata syncs. Wait for main.cpp's retry log,
                    # which is emitted only after runPipeWire has unwound and
                    # Runtime cleanup is complete, before stopping the server.
                    wait_for_daemon_log(
                        daemon_process, daemon_log, daemon_log_offset,
                        "; retrying in 2 seconds",
                        "selected-source Runtime teardown")

                if server_restart:
                    print("Stopping private PipeWire server…", flush=True)
                    stop(server, "private PipeWire server")
                    server = None
                    socket = runtime / "pipewire-0"
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline and socket.exists():
                        time.sleep(0.05)
                    if socket.exists():
                        raise RuntimeError(
                            "PipeWire socket remained after server shutdown")
                    print("Restarting private server and source…", flush=True)
                    server = start_private_pipewire(
                        pipewire, pw_config, env, pipewire_log, runtime)
                    if playback_server_restart:
                        wireplumber_process = start_wireplumber(
                            wireplumber, env, wireplumber_log)
                        time.sleep(0.5)
                        if wireplumber_process.poll() is not None:
                            raise RuntimeError(
                                "private WirePlumber policy process exited "
                                "after PipeWire server restart")
                        null_target_manager = subprocess.Popen(
                            [str(pw_cli)], env=env, text=True,
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=pw_cli_log)
                        null_target_manager.stdin.write(
                            f"create-node adapter {target_properties}\n")
                        null_target_manager.stdin.flush()
                        deadline = time.monotonic() + 8
                        target_available = False
                        while time.monotonic() < deadline:
                            graph = pipewire_dump_objects(
                                run([str(pw_dump)], env, timeout=5).stdout)
                            target_available = any(
                                item.get("type", "").endswith(":Node") and
                                item.get("info", {}).get(
                                    "props", item.get("props", {}))
                                .get("node.name") == "skyapo.test.null-target"
                                for item in graph)
                            if target_available:
                                break
                            time.sleep(0.05)
                        if not target_available:
                            raise RuntimeError(
                                "PipeWire playback target did not return after "
                                "server restart")
                        metadata = shutil.which("pw-metadata")
                        if not metadata:
                            raise RuntimeError(
                                "pw-metadata is required for playback server "
                                "restart recovery")
                        run([metadata, "-n", "default", "0",
                             "default.audio.sink",
                             '{"name":"skyapo.test.null-target"}',
                             "Spa:String:JSON"], env, timeout=5)

                replacement_name = ("skyapo.test.renamed"
                                    if stable_id_replug else device_name)
                replacement_serial = ("SKYAPO-TEST-SERIAL-42"
                                      if stable_id_replug else None)
                print("Daemon detected removal; publishing replacement "
                      f"source {replacement_name}…", flush=True)
                source_process = start_test_source(
                    source, mono, env, source_log, name=replacement_name,
                    serial=replacement_serial)
                deadline = time.monotonic() + 15
                relinked_status = ""
                while time.monotonic() < deadline:
                    if source_process.poll() is not None:
                        raise RuntimeError(
                            "replacement deterministic source exited")
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited while recovering selected source "
                            f"(exit status {daemon_process.returncode})")
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    relinked_status = result.stdout
                    if (result.returncode == 0 and
                            "Daemon: streaming" in relinked_status and
                            "SkyAPO Virtual Mic" in relinked_status and
                            f"Selected device: {replacement_name}" in
                            relinked_status and
                            expected_links in relinked_status):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "daemon did not recover after the same stable-name "
                        f"source returned:\n{relinked_status}")
                new_virtual_id = virtual_source_id(relinked_status)
                if not server_restart and new_virtual_id == old_virtual_id:
                    raise RuntimeError(
                        "virtual source node ID did not change after source "
                        "recovery; expected a recreated PipeWire node")
                assert_virtual_source(pw_dump, env, new_virtual_id)
                current_after_replug = run(
                    [str(cli), "device", "current"], env).stdout
                if replacement_name not in current_after_replug:
                    raise RuntimeError(
                        "stable selection did not resolve the renamed node: "
                        f"{current_after_replug}")
                print(f"Selected source/server recovery ({mode}) returned "
                      f"SkyAPO Virtual Mic at node {new_virtual_id} "
                      f"(previous graph ID {old_virtual_id}; IDs may be reused "
                      f"after a server restart) with {channel_count}/"
                      f"{channel_count} capture links.")
                recovered_status = relinked_status

                if playback_server_restart:
                    deadline = time.monotonic() + 15
                    playback_recovered_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        playback_recovered_status = result.stdout
                        if (result.returncode == 0 and
                                "Playback graph: streaming / streaming, "
                                "internal links 2/2, target links 2/2 channels"
                                in playback_recovered_status and
                                "PipeWire default sink: skyapo.playback" in
                                playback_recovered_status):
                            break
                        time.sleep(0.1)
                    else:
                        raise RuntimeError(
                            "playback graph did not recover after the private "
                            "PipeWire server restart:\n"
                            f"{playback_recovered_status}")
                    recovery_consumer_args = [
                        str(consumer), "--expected-db", "-6",
                        "--expected-rate", str(sample_rate),
                        "--duration-ms", "2500", "--capture-node",
                        "skyapo.test.null-target", "--signal-ready",
                        "--stream-capture-sink",
                        "--name", "skyapo.test.consumer.after-server-restart"]
                    second_source_process = subprocess.Popen(
                        [str(playback_client)], env=env, text=True,
                        stdout=source_log, stderr=subprocess.STDOUT)
                    wait_for_playback_application_route(
                        second_source_process, pw_dump, env, channel_count)
                    playback_consumer = subprocess.Popen(
                        recovery_consumer_args, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    ready_output = wait_for_consumer_ready(
                        playback_consumer,
                        "post-restart independent playback monitor")
                    try:
                        remaining_output, _ = playback_consumer.communicate(
                            timeout=10)
                    except subprocess.TimeoutExpired as error:
                        raise RuntimeError(
                            "post-restart playback monitor did not finish") from error
                    captured_playback = ready_output + remaining_output
                    ratio = re.search(
                        r"RMS ratio to expected: ([0-9.]+)",
                        captured_playback)
                    if (playback_consumer.returncode != 0 or not ratio or
                            abs(float(ratio.group(1)) - 1.0) >= 0.03):
                        graph_result = run([str(pw_dump)], env, timeout=5,
                                           check=False)
                        graph_diagnostic = (
                            summarize_graph_dump(graph_result.stdout)
                            if graph_result.returncode == 0 else
                            f"pw-dump failed ({graph_result.returncode}): "
                            f"{graph_result.stderr}")
                        tap_diagnostics = []
                        source_log.flush()
                        source_log.seek(0)
                        source_format_events = [
                            line.rstrip() for line in source_log
                            if "test playback stream: negotiated format=" in
                            line]
                        source_log.seek(0, os.SEEK_END)
                        if server is not None and server.poll() is None:
                            for tap_node, tap_expected_db in (
                                    ("skyapo.test.playback-app", "0"),
                                    ("skyapo.playback.dsp", "-6"),
                                    ("skyapo.playback.stream", "-6")):
                                tap = subprocess.Popen(
                                    [str(consumer), "--expected-db",
                                     tap_expected_db,
                                     "--expected-rate", str(sample_rate),
                                     "--duration-ms", "1000", "--capture-node",
                                     tap_node, "--signal-ready"],
                                    env=env, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
                                try:
                                    ready = wait_for_consumer_ready(
                                        tap, f"diagnostic tap {tap_node}")
                                    tail, _ = tap.communicate(timeout=5)
                                    tap_diagnostics.append(
                                        f"{tap_node} (expected {tap_expected_db} "
                                        f"dB, exit {tap.returncode}):\n"
                                        f"{ready}{tail}")
                                except Exception as tap_error:
                                    stop(tap, f"diagnostic tap {tap_node}")
                                    tap_diagnostics.append(
                                        f"{tap_node} tap failed: {tap_error}")
                        raise RuntimeError(
                            "independent playback recording after PipeWire "
                            "server restart did not measure the expected "
                            f"-6 dB output:\n{captured_playback}\n"
                            f"{playback_recovered_status}\n"
                            "Playback source negotiated formats:\n" +
                            "\n".join(source_format_events) + "\n"
                            "PipeWire graph at failed measurement:\n"
                            f"{graph_diagnostic}\n"
                            "Signal taps along the playback path:\n" +
                            "\n".join(tap_diagnostics))
                    stop(second_source_process,
                         "post-restart ordinary playback stream")
                    second_source_process = None
                    playback_consumer = None
                    print("PipeWire server playback recovery passed: the "
                          "WirePlumber route returned and independent sink "
                          "monitor measured the expected -6 dB DSP level "
                          f"(ratio {float(ratio.group(1)):.6f}).")

            expected_db = (-6.020599913 if convolution else
                           (-6.0 if lv2_pdc_recovery else
                           (0.0 if (lv2_pdc_final or delay_pdc) else
                           (-12.020599913 if
                            (latency_plugin or clap_latency_change or
                             vst2_latency_change) else
                            (-3.0 if transition_format else
                             (0.0 if playback_stage else -6.0))))))
            if plugin_live:
                live_value = "0.75" if (vst2_live or mode == "plugin-live-param") else "0.25"
                changed = run([str(cli), "plugin", "set",
                               live_plugin_id, live_parameter, live_value], env)
                if f"Updated {live_plugin_id} parameter {live_parameter} " \
                   f"to {float(live_value):.6f}" \
                        not in changed.stdout:
                    raise RuntimeError(
                        f"live {mode} parameter update was not acknowledged: "
                        f"{changed.stdout}")
                expected_db = (-2.498774732 if vst2_live else
                               (-8.498774732 if mode == "plugin-live-param"
                                else -18.041199913))
                if mode == "plugin-live-param":
                    reloaded = run([str(cli), "config", "reload"], env)
                    if "Config reload succeeded" not in reloaded.stdout:
                        raise RuntimeError(
                            "CLAP state did not survive live graph rebuild:\n"
                            f"{reloaded.stdout}")
                    reloaded_status = run([str(cli), "status"], env).stdout
                    for expected in (
                            "Daemon: streaming", "Callback allocations: 0",
                            "Callback deallocations: 0"):
                        if expected not in reloaded_status:
                            raise RuntimeError(
                                "CLAP state reload violated runtime safety "
                                f"expectation {expected!r}:\n{reloaded_status}")
            if plugin_bypass:
                changed = run([str(cli), "plugin", "bypass",
                               live_plugin_id, "on"], env)
                if changed.stdout != f"Bypassed {live_plugin_id}\n":
                    raise RuntimeError(
                        f"plugin bypass was not acknowledged: {changed.stdout}")
                # The fixture normally halves samples; bypass must leave only
                # the configured -6 dB Preamp in the independently recorded path.
                expected_db = -6.0
            if include_reload:
                include_child.write_text("Preamp: -3 dB\n")
                deadline = time.monotonic() + 8
                reloaded = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    reloaded = result.stdout
                    if (result.returncode == 0 and
                            "DSP amplitude ratio: 0.707946" in reloaded):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "editing an active Include did not reload the graph:\n"
                        f"{reloaded}")

                include_child.write_text("UnsupportedInInclude: true\n")
                deadline = time.monotonic() + 8
                rejected = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    rejected = result.stdout
                    if ("Config reload error:" in rejected and
                            "DSP amplitude ratio: 0.707946" in rejected):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "invalid Include reload did not preserve the last "
                        f"valid graph:\n{rejected}")

                include_child.write_text(
                    "Filter: ON IIR Order 1 Coefficients 1 0 0 -0.5\n")
                deadline = time.monotonic() + 8
                rejected_iir = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    rejected_iir = result.stdout
                    if ("Config reload error:" in rejected_iir and
                            "IIR a0 coefficient must be nonzero" in
                            rejected_iir and
                            "DSP amplitude ratio: 0.707946" in rejected_iir):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "unsafe IIR Include reload did not retain the last "
                        f"valid graph:\n{rejected_iir}")
                shutil.rmtree(include_directory)
                deadline = time.monotonic() + 8
                missing_include = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    missing_include = result.stdout
                    if ("Config reload error:" in missing_include and
                            "cannot open config:" in missing_include and
                            "DSP amplitude ratio: 0.707946" in
                            missing_include):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "removing the Include directory did not preserve the "
                        f"last valid graph:\n{missing_include}")

                include_directory.mkdir()
                include_child.write_text("Preamp: -2 dB\n")
                deadline = time.monotonic() + 8
                recovered_include = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    recovered_include = result.stdout
                    if (result.returncode == 0 and
                            "Config reload error:" not in recovered_include and
                            "DSP amplitude ratio: 0.794328" in
                            recovered_include):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "recreated Include directory did not reload:\n"
                        f"{recovered_include}")

                for gain in ("-1", "-4", "-2", "-5", "-2"):
                    include_child.write_text(f"Preamp: {gain} dB\n")
                    burst_reload = run(
                        [str(cli), "config", "reload"], env, timeout=8)
                    if "Config reload succeeded" not in burst_reload.stdout:
                        raise RuntimeError(
                            "rapid valid config reload failed:\n"
                            f"{burst_reload.stdout}")
                include_child.write_text("UnsupportedBurstCommand: true\n")
                burst_rejected = run(
                    [str(cli), "config", "reload"], env, timeout=8,
                    check=False)
                if (burst_rejected.returncode == 0 or
                        "Config reload failed; keeping last valid graph" not in
                        burst_rejected.stdout + burst_rejected.stderr):
                    raise RuntimeError(
                        "invalid reload burst did not retain last graph:\n"
                        f"{burst_rejected.stdout}{burst_rejected.stderr}")
                include_child.write_text("Preamp: -2 dB\n")
                burst_recovered = run(
                    [str(cli), "config", "reload"], env, timeout=8)
                if "Config reload succeeded" not in burst_recovered.stdout:
                    raise RuntimeError(
                        "daemon failed to reload after rapid invalid edit:\n"
                        f"{burst_recovered.stdout}")
                burst_status = run([str(cli), "status"], env).stdout
                if "Daemon: streaming" not in burst_status:
                    raise RuntimeError(
                        "daemon stopped during rapid reload burst:\n"
                        f"{burst_status}")

                future_include = include_directory / "future.txt"
                included_config.write_text(
                    "Include: includes/future.txt\n")
                deadline = time.monotonic() + 8
                rejected_future = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    rejected_future = result.stdout
                    if ("Config reload error:" in rejected_future and
                            "cannot open config:" in rejected_future and
                            "DSP amplitude ratio: 0.794328" in rejected_future):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "missing newly referenced Include did not preserve "
                        f"the last valid graph:\n{rejected_future}")
                future_include.write_text("Preamp: -4 dB\n")
                deadline = time.monotonic() + 8
                recovered_future = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    recovered_future = result.stdout
                    if (result.returncode == 0 and
                            "Config reload error:" not in recovered_future and
                            "DSP amplitude ratio: 0.630957" in
                            recovered_future):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "creating a newly referenced Include did not recover "
                        f"the graph:\n{recovered_future}")
                print("Include edit reloaded to -3 dB; invalid edit and "
                      "unsupported-command and zero-a0 IIR edits plus "
                      "directory removal retained it; directory recreation "
                      "reloaded to -2 dB. Five successive live transitions, "
                      "invalid reloads, a valid recovery, and a newly "
                      "created missing Include recovery also passed.")
                expected_db = -4.0

            if vst3_nonstd_latency_failure:
                deadline = time.monotonic() + 8
                latency_error_status = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    latency_error_status = result.stdout
                    if ("Plugin latency update error: unknown non-standard "
                            "exception during plugin latency refresh" in
                            latency_error_status):
                        break
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited after a non-standard VST3 latency "
                            "exception")
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "non-standard VST3 latency exception was not contained "
                        f"by the daemon:\n{latency_error_status}")

            consumer_args = [str(consumer)]
            if mono:
                consumer_args.append("--mono")
            elif surround51:
                consumer_args.append("--surround51")
            elif surround71:
                consumer_args.append("--surround71")
            consumer_args += ["--expected-rate", str(sample_rate)]
            if wireplumber_playback:
                # Exercise WirePlumber's real application-capture stream path.
                # A pw_filter with Stream/Input/Audio properties is not a
                # client stream and can leave one channel paused during the
                # virtual source's PortConfig negotiation.
                consumer_args.append("--stream-capture")
            if lv2_pdc_config_commit_race:
                consumer_args.append("--accept-any-audio")
            if latency_probe:
                consumer_args += ["--accept-any-audio", "--measure-source-lag"]
            if lv2_pdc_final or delay_pdc:
                consumer_args += ["--expect-identical-channels"]
            if delay_pdc:
                # The consumer's FL and FR links become active independently.
                # Start its measurement only after both are connected so the
                # link-activation transient is not mistaken for PDC skew.
                consumer_args.append("--signal-connected")
            if callback_transition_exception:
                callback_exception_config.write_text("Preamp: -3 dB\n")
                transition_reload = run(
                    [str(cli), "config", "reload"], env, timeout=10,
                    check=False)
                if (transition_reload.returncode != 0 or
                        "Config reload succeeded" not in
                        transition_reload.stdout):
                    raise RuntimeError(
                        "could not start the long DSP crossfade for callback "
                        f"exception test:\n{transition_reload.stdout}"
                        f"{transition_reload.stderr}")
                # Replace the valid edit before the debounced watcher can
                # reload it after the injected fault. The pending graph has
                # already been accepted, and the control reload below must
                # reject this bad candidate without clearing the sticky fault.
                callback_exception_config.write_text(
                    "UnsupportedCallbackRecoveryCommand: true\n")
                deadline = time.monotonic() + 8
                transition_fault = ""
                while time.monotonic() < deadline:
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    transition_fault = result.stdout
                    if ("DSP state: paused (realtime processing exception; "
                            "output silence)" in transition_fault):
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError(
                        "DSP exception did not interrupt an in-progress "
                        f"crossfade:\n{transition_fault}")

            if plugin_process_failure or callback_process_exception:
                consumer_args.append("--expect-silent")
            elif plugin_chain:
                consumer_args += ["--expected-db", str(expected_db)]
            elif (include_reload or transition_format or delay_pdc or
                  convolution or playback_stage):
                consumer_args += ["--expected-db", str(expected_db)]
            if mode == "playback-preamp":
                # The playback verification consumer is also the selected
                # playback target. Give this capture verification its own node
                # name so the playback graph cannot route into and sum with
                # the virtual-microphone recording sink.
                consumer_args += ["--name", "skyapo.test.capture-monitor"]
            consumer_graph = ""
            consumer_graph_summary = ""
            try:
                if lv2_pdc_final or mode == "playback-wireplumber":
                    consumer_process = subprocess.Popen(
                        consumer_args, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    time.sleep(0.2)
                    consumer_graph = run(
                        [str(pw_dump)], env, timeout=5,
                        check=False).stdout
                    consumer_graph_summary = summarize_graph_dump(
                        consumer_graph)
                    if env.get("SKYAPO_TEST_TRACE_PIPEWIRE_GRAPH") == "1":
                        print("PipeWire graph while virtual-microphone consumer "
                              f"is connected:\n{consumer_graph_summary}")
                    consumer_stdout, consumer_stderr = consumer_process.communicate(
                        timeout=12)
                    captured = subprocess.CompletedProcess(
                        consumer_args, consumer_process.returncode,
                        consumer_stdout, consumer_stderr)
                    if captured.returncode:
                        raise RuntimeError(
                            f"consumer exited with status {captured.returncode}: "
                            f"{' '.join(consumer_args)}\n"
                            f"stdout:\n{captured.stdout}\nstderr:\n"
                            f"{captured.stderr}")
                else:
                    captured = run(consumer_args, env, timeout=12)
            except RuntimeError as error:
                if not (lv2_pdc_final or mode == "playback-wireplumber"):
                    raise
                current_status = run([str(cli), "status"], env,
                                     timeout=5, check=False)
                graph = run([str(pw_dump)], env, timeout=5, check=False)
                try:
                    graph_summary = summarize_graph_dump(graph.stdout)
                except (json.JSONDecodeError, TypeError):
                    graph_summary = graph.stdout
                raise RuntimeError(
                    f"{error}\n--- status at consumer failure ---\n"
                    f"{current_status.stdout}\n"
                    f"--- pw-dump during consumer capture ---\n"
                    f"{consumer_graph_summary}\n"
                    f"--- pw-dump at consumer failure ---\n"
                    f"{graph_summary}\n{graph.stderr}") from error
            if mode == "playback-wireplumber":
                trace_graph_if_requested(
                    "WirePlumber virtual-microphone consumer capture",
                    pw_dump, env)
            if lv2_pdc_final:
                final_status = run([str(cli), "status"], env).stdout
                if ("Plugin-reported latency sum: 32 samples "
                        "(sum only; not end-to-end latency)" not in final_status or
                        "Plugin latency refreshes: 1" not in final_status or
                        "Plugin latency compensation: active "
                        "(plugin-reported latency only)" not in final_status):
                    raise RuntimeError(
                        "runtime did not refresh LV2 latency after the PDC "
                        f"transition recording:\n{final_status}")
                status = final_status
            if f"Sample rate: {sample_rate} Hz" not in captured.stdout:
                raise RuntimeError(
                    f"independent consumer did not negotiate {sample_rate} Hz:\n"
                    f"{captured.stdout}")
            match = re.search(r"RMS ratio to expected: ([0-9.]+)",
                              captured.stdout)
            if (not latency_probe and not plugin_process_failure and
                    not callback_process_exception and
                    (not match or abs(float(match.group(1)) - 1.0) >= 0.03)):
                raise RuntimeError(
                    f"capture output did not report expected "
                    f"{expected_db:g} dB:\n"
                    f"{captured.stdout}")
            if plugin_process_failure:
                silence = re.search(r"RMS: ([0-9.]+)", captured.stdout)
                if not silence or float(silence.group(1)) >= 1.0e-6:
                    raise RuntimeError(
                        "failed CLAP process did not silence the recorded "
                        f"virtual-microphone stream:\n{captured.stdout}")
                if daemon_process.poll() is not None:
                    raise RuntimeError(
                        "skyapod exited after the independent client recorded "
                        "the failed-plugin silence")
                status = run([str(cli), "status"], env).stdout
                plugin_failure_visible = (
                    "Plugin latency update error: unknown non-standard "
                    "exception during plugin latency refresh" in status
                    if vst3_nonstd_latency_failure else
                    failing_plugin_id in status)
                if ("Daemon: streaming" not in status or
                        not plugin_failure_visible):
                    raise RuntimeError(
                        "failed plugin status/daemon state was not retained "
                        f"after recording:\n{status}")
                if vst3_exception_failure and (
                        "Processed blocks: 0" in status or
                        "Callback allocations: 1" not in status or
                        "Callback deallocations: 1" not in status):
                    raise RuntimeError(
                        "VST3 exception did not occur inside an exercised DSP "
                        "callback with the expected single exception-handling "
                        f"allocation/deallocation:\n{status}")
                failure_description = (
                    "VST3 non-standard latency exception" if
                    vst3_nonstd_latency_failure else
                    "VST3 exception" if vst3_exception_failure else
                    "CLAP/VST3 process error")
                print(f"{failure_description} was surfaced, the daemon and virtual "
                      "source stayed live, and an independent consumer "
                      f"measured silence (RMS {float(silence.group(1)):.8f}).")
                if vst3_exception_failure:
                    print("The invalid plugin exception incurred one audited "
                          "allocation/deallocation for C++ exception handling; "
                          "this fault path is not realtime-allocation-free.")
            if callback_process_exception:
                silence = re.search(r"RMS: ([0-9.]+)", captured.stdout)
                if not silence or float(silence.group(1)) >= 1.0e-6:
                    raise RuntimeError(
                        "callback exception did not silence the independent "
                        f"virtual-microphone recording:\n{captured.stdout}")
                if daemon_process.poll() is not None:
                    raise RuntimeError(
                        "daemon exited after an exception crossed the DSP "
                        "callback boundary")
                fault_status = run([str(cli), "status"], env).stdout
                if ("Daemon: streaming" not in fault_status or
                        "DSP state: paused (realtime processing exception; "
                        "output silence)" not in fault_status or
                        "DSP callback error: processing exception" not in
                        fault_status):
                    raise RuntimeError(
                        "callback exception did not publish a persistent "
                        f"fail-closed status:\n{fault_status}")

                callback_exception_config.write_text(
                    "UnsupportedCallbackRecoveryCommand: true\n")
                rejected = run([str(cli), "config", "reload"], env,
                               timeout=10, check=False)
                if (rejected.returncode == 0 or
                        "Config reload failed" not in
                        rejected.stdout + rejected.stderr):
                    raise RuntimeError(
                        "invalid config was not rejected while recovering "
                        "from a DSP callback exception:\n"
                        f"{rejected.stdout}{rejected.stderr}")
                still_faulted = run([str(cli), "status"], env,
                                    timeout=5, check=False)
                if (still_faulted.returncode != 0 or
                        "DSP state: paused (realtime processing exception; "
                        "output silence)" not in still_faulted.stdout or
                        "DSP callback error: processing exception" not in
                        still_faulted.stdout):
                    raise RuntimeError(
                        "invalid config incorrectly cleared callback fault:\n"
                        f"{still_faulted.stdout}{still_faulted.stderr}")
                retained_silence = run(
                    [str(consumer), "--expect-silent",
                     "--expected-rate", str(sample_rate)], env, timeout=12)
                retained_rms = re.search(r"RMS: ([0-9.]+)",
                                         retained_silence.stdout)
                if (not retained_rms or
                        float(retained_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "invalid recovery config resumed the faulted audio "
                        f"graph:\n{retained_silence.stdout}")

                callback_exception_config.write_text("Preamp: -6 dB\n")
                reloaded = run([str(cli), "config", "reload"], env)
                if "Config reload succeeded" not in reloaded.stdout:
                    raise RuntimeError(
                        "valid config reload did not recover a faulted "
                        f"callback graph:\n{reloaded.stdout}")
                recovered_status = run([str(cli), "status"], env).stdout
                if ("Daemon: streaming" not in recovered_status or
                        "DSP state: running" not in recovered_status or
                        "DSP callback error" in recovered_status):
                    raise RuntimeError(
                        "callback fault recovery status is incomplete or "
                        f"remains faulted:\n{recovered_status}")
                recovered_consumer_args = [
                    argument for argument in consumer_args
                    if argument != "--expect-silent"]
                recovered_consumer_args += ["--expected-db", "-6"]
                recovered_capture = run(recovered_consumer_args, env, timeout=12)
                recovered_ratio = re.search(
                    r"RMS ratio to expected: ([0-9.]+)",
                    recovered_capture.stdout)
                if (not recovered_ratio or
                        abs(float(recovered_ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        "recovered graph did not produce the expected -6 dB "
                        f"audio:\n{recovered_capture.stdout}")
                print("Injected C++ exception was contained at the PipeWire "
                      "callback boundary: daemon/source survived, independent "
                      f"consumer measured silence{' during a partially completed '
                      'crossfade' if callback_transition_exception else ''}; "
                      "an invalid recovery config "
                      "kept it silenced; valid config reload restored -6 dB "
                      f"processing (ratio "
                      f"{float(recovered_ratio.group(1)):.6f}).")
            if latency_probe:
                latency_match = re.search(
                    r"Source callback to virtual-consumer callback latency: "
                    r"median (\d+) us, p95 (\d+) us, n=(\d+) at (\d+) Hz",
                    captured.stdout)
                if (not latency_match or int(latency_match.group(3)) < 4 or
                        int(latency_match.group(4)) != sample_rate):
                    raise RuntimeError(
                        "source/consumer timestamp latency measurement "
                        f"failed:\n{captured.stdout}")
                print("Private PipeWire source callback → virtual-consumer "
                      "callback processing delay: median "
                      f"{latency_match.group(1)} µs, p95 "
                      f"{latency_match.group(2)} µs over "
                      f"{latency_match.group(3)} impulses. This excludes "
                      "physical ADC/device and application latency.")
            if lv2_pdc_final:
                channel_match = re.search(
                    r"Inter-channel difference ratio: ([0-9.]+)",
                    captured.stdout)
                if (not channel_match or
                        float(channel_match.group(1)) >= 0.01):
                    raise RuntimeError(
                        "recorded stereo channels were not aligned after the "
                        f"mono latency plugin/PDC path:\n{captured.stdout}")
            print(f"Private PipeWire {mode} recovery and capture passed "
                  f"(destroyed link {link_id}).")
            print(status.rstrip())
            print(captured.stdout.rstrip())
            stopped = run([str(cli), "stop"], env, timeout=8)
            if "Stopping skyapod" not in stopped.stdout:
                raise RuntimeError(f"daemon stop was not acknowledged: {stopped.stdout}")
            daemon_process.wait(timeout=5)
            if daemon_process.returncode != 0:
                raise RuntimeError(
                    f"skyapod exited with status {daemon_process.returncode}")
            if wireplumber_playback:
                metadata = shutil.which("pw-metadata")
                if not metadata:
                    raise RuntimeError(
                        "WirePlumber playback test requires pw-metadata")
                default_metadata = run(
                    [metadata, "-n", "default"], env, timeout=5).stdout
                expected_defaults = (
                    {"skyapo.test.null-target-alt"}
                    if playback_systemd_user_override else
                    {"skyapo.test.null-target-alt"}
                    if mode in (
                        "playback-wireplumber",
                        "playback-wireplumber-target-recovery-stable-id") else
                    {"skyapo.test.null-target"})
                if stable_playback_target:
                    # WirePlumber may resolve the restored configured default
                    # to its stable-identity replacement, or keep the fallback
                    # sink active; both are valid after the old node name was
                    # recreated with a new name. The configured choice itself
                    # is checked exactly below.
                    expected_defaults.add(replacement_target_name)
                restored_default = metadata_node_name(
                    default_metadata, "default.audio.sink")
                if restored_default not in expected_defaults:
                    raise RuntimeError(
                        "orderly daemon shutdown did not restore the original "
                        "PipeWire default sink metadata; expected one of "
                        f"{sorted(expected_defaults)}, found "
                        f"{restored_default!r}:\n{default_metadata}")
                if ((stable_playback_target or playback_systemd_crash_restart) and
                        not playback_systemd_user_override and
                        metadata_node_name(default_metadata,
                                           "default.configured.audio.sink") !=
                        initial_configured_sink):
                    raise RuntimeError(
                        "orderly daemon shutdown did not restore WirePlumber's "
                        "configured playback default:\n" + default_metadata)
                if playback_systemd_user_override:
                    if (metadata_node_name(default_metadata,
                                           "default.audio.sink") !=
                            "skyapo.test.null-target-alt" or
                            metadata_node_name(
                                default_metadata,
                                "default.configured.audio.sink") !=
                            "skyapo.test.null-target-alt"):
                        raise RuntimeError(
                            "daemon overwrote the user's playback default "
                            "changed while it was down:\n" + default_metadata)
                if playback_systemd_crash_restart:
                    route_state = (state_home / "skyapo" /
                                   "playback-default-route-v1")
                    if route_state.exists():
                        raise RuntimeError(
                            "clean daemon shutdown left a stale playback "
                            f"route recovery file: {route_state}")
            state_layout = {
                "plugin-live-param": ("clap-state", "*.clapstate", "CLAP"),
                "clap-state-restart": ("clap-state", "*.clapstate", "CLAP"),
                "lv2-state-restart": ("lv2-state", "*.ttl", "LV2"),
                "vst3-state-restart": (
                    "vst3-state", "*.vst3state", "VST3"),
            }
            if mode in state_layout:
                state_directory, state_pattern, state_format = state_layout[mode]
                state_dir = state_home / "skyapo" / state_directory
                sidecars = list(state_dir.glob(state_pattern))
                if len(sidecars) != 1:
                    raise RuntimeError(
                        f"orderly {state_format} daemon stop did not save exactly one "
                        f"state sidecar: {sidecars}")
                if state_dir.stat().st_mode & 0o077:
                    raise RuntimeError(
                        f"{state_format} state directory permissions are not 0700")
                if sidecars[0].stat().st_mode & 0o077:
                    raise RuntimeError(
                        f"{state_format} state sidecar permissions are not 0600")
                daemon_process = subprocess.Popen(
                    [str(daemon), "--config", str(dsp_config)], env=env,
                    stdout=daemon_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 15
                restored_status = ""
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError(
                            "skyapod exited during CLAP state restoration")
                    result = run([str(cli), "status"], env, timeout=5,
                                 check=False)
                    restored_status = result.stdout
                    if (result.returncode == 0 and
                            "Daemon: streaming" in restored_status and
                            "SkyAPO Virtual Mic" in restored_status and
                            "Active capture links: 2/2" in restored_status and
                            "Callback allocations: 0" in restored_status and
                            "Callback deallocations: 0" in restored_status):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        f"daemon did not resume the {state_format} state graph:\n"
                        f"{restored_status}")
                restored_capture = run(
                    [str(consumer), "--expected-rate", str(sample_rate),
                     "--expected-db", str(expected_db)], env, timeout=12)
                restored_ratio = re.search(
                    r"RMS ratio to expected: ([0-9.]+)",
                    restored_capture.stdout)
                if (not restored_ratio or
                        abs(float(restored_ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        f"fresh daemon did not restore saved {state_format} audio state:\n"
                        f"{restored_capture.stdout}")
                valid_state = sidecars[0].read_bytes()
                if state_format == "LV2":
                    corrupted_state = b"not valid Turtle state @"
                else:
                    corrupted_state = bytearray(valid_state)
                    corrupted_state[-1] ^= 0x01
                sidecars[0].write_bytes(corrupted_state)
                rejected_reload = run(
                    [str(cli), "config", "reload"], env, check=False)
                if (rejected_reload.returncode == 0 or
                        "Config reload failed; keeping last valid graph" not in
                        rejected_reload.stdout + rejected_reload.stderr or
                        (state_format != "LV2" and "checksum mismatch" not in
                         rejected_reload.stdout + rejected_reload.stderr)):
                    raise RuntimeError(
                        f"runtime accepted corrupt {state_format} state sidecar:\n"
                        f"{rejected_reload.stdout}{rejected_reload.stderr}")
                retained_status = run([str(cli), "status"], env).stdout
                if "Daemon: streaming" not in retained_status:
                    raise RuntimeError(
                        f"corrupt {state_format} state stopped the active graph:\n"
                        f"{retained_status}")
                retained_capture = run(
                    [str(consumer), "--expected-rate", str(sample_rate),
                     "--expected-db", str(expected_db)], env, timeout=12)
                retained_ratio = re.search(
                    r"RMS ratio to expected: ([0-9.]+)",
                    retained_capture.stdout)
                if (not retained_ratio or
                        abs(float(retained_ratio.group(1)) - 1.0) >= 0.03):
                    raise RuntimeError(
                        f"corrupt {state_format} state did not retain the prior live DSP "
                        f"output:\n{retained_capture.stdout}")
                sidecars[0].write_bytes(valid_state)
                recovered_reload = run(
                    [str(cli), "config", "reload"], env, check=False)
                if "Config reload succeeded" not in recovered_reload.stdout:
                    raise RuntimeError(
                        f"restored {state_format} sidecar could not reload:\n"
                        f"{recovered_reload.stdout}")
                stopped_again = run([str(cli), "stop"], env, timeout=8)
                if "Stopping skyapod" not in stopped_again.stdout:
                    raise RuntimeError(
                        "restored daemon stop was not acknowledged")
                daemon_process.wait(timeout=5)
                if daemon_process.returncode != 0:
                    raise RuntimeError(
                        f"restored {state_format} skyapod exited with status "
                        f"{daemon_process.returncode}")
                print(f"{state_format} state survived orderly shutdown/recreation; "
                      f"independent capture: {restored_capture.stdout.strip()}")
        except Exception:
            for label, log in (("PipeWire server", pipewire_log),
                               ("WirePlumber policy", wireplumber_log),
                               ("persistent pw-cli target manager", pw_cli_log),
                               ("test source", source_log),
                               ("unrelated playback consumer",
                                unrelated_playback_log),
                               ("skyapod", daemon_log)):
                log.flush()
                log.seek(0)
                details = log.read()
                if details:
                    print(f"--- {label} log ---\n{details}", file=sys.stderr)
            for label, process in (("PipeWire server", server),
                                   ("WirePlumber policy", wireplumber_process),
                                   ("persistent pw-cli target manager",
                                    null_target_manager)):
                if process is not None:
                    print(f"{label} process exit status at failure: "
                          f"{process.poll()}", file=sys.stderr)
            raise
        finally:
            if wireplumber_paused and wireplumber_process is not None:
                os.killpg(wireplumber_process.pid, signal.SIGCONT)
                wireplumber_paused = False
            try:
                shutdown(((daemon_process, "skyapod"),
                          (source_process, "test source"),
                          (second_source_process, "alternate test source"),
                          (unrelated_playback_process,
                           "unrelated playback test stream"),
                          (wireplumber_process, "WirePlumber policy"),
                          (null_target_manager, "null playback target manager"),
                          (playback_consumer, "playback test consumer"),
                          (post_crash_playback_consumer,
                           "post-crash playback test consumer"),
                          (unrelated_playback_consumer,
                           "unrelated playback test consumer"),
                          (transition_consumer,
                           "continuous playback transition consumer"),
                          (rollback_consumer,
                           "playback output rollback monitor consumer"),
                          (transition_fault_consumer,
                           "first-fault playback monitor consumer"),
                          (boundary_consumer, "PDC boundary test consumer"),
                          (race_monitor, "PDC config-commit race monitor"),
                          (config_reload_process,
                           "PDC config-commit reload client"),
                          (server, "PipeWire server")))
            except Exception as cleanup_error:
                print(f"E2E cleanup warning: {cleanup_error}", file=sys.stderr)
            finally:
                for log in (daemon_log, source_log, playback_consumer_log,
                            unrelated_playback_log, race_monitor_log,
                            pipewire_log, wireplumber_log, pw_cli_log):
                    log.flush()
                    log.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # include subprocess diagnostics in CTest output
        print(f"PipeWire E2E failure: {error}", file=sys.stderr)
        sys.exit(1)
