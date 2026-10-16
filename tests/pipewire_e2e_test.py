#!/usr/bin/env python3
"""Launch an isolated PipeWire graph and verify captured DSP output."""

import os
import json
import pathlib
import re
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from pprint import pformat


def run(args, env, timeout=8, check=True):
    result = subprocess.run(
        args, env=env, text=True, capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


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


def start_systemd_daemon(systemd_run, systemctl, daemon, config, env,
                         unit_template):
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
        f"--property=RestartSec={restart_sec}",
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
    objects = json.loads(run([str(pw_dump)], env, timeout=5).stdout)
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
        raise RuntimeError(
            "could not find the physical-capture → SkyAPO link in pw-dump")
    link_id = str(candidates[0])
    run([str(pw_cli), "destroy", link_id], env, timeout=5)
    return link_id


def virtual_source_id(status):
    match = re.search(r"^Virtual node: (\d+)", status, re.MULTILINE)
    if not match:
        raise RuntimeError("daemon status did not expose the virtual node ID")
    return match.group(1)


def assert_virtual_source(pw_dump, env, expected_id):
    objects = json.loads(run([str(pw_dump)], env, timeout=5).stdout)
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


def summarize_graph_dump(raw):
    objects = json.loads(raw)
    nodes = set()
    summary = []
    for item in objects:
        info = item.get("info", {})
        props = info.get("props", item.get("props", {}))
        if item.get("type", "").endswith(":Node"):
            name = props.get("node.name", "")
            if name in ("skyapo.virtual_mic", "skyapo.test.consumer",
                        "skyapo.test.input"):
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


def start_private_pipewire(pipewire, config, env, logs, runtime):
    process = subprocess.Popen(
        [str(pipewire), "--config", str(config)], env=env,
        stdout=logs, stderr=subprocess.STDOUT)
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
                      latency_probe=False):
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
    return subprocess.Popen(
        args, env=env,
        stdout=source_log, stderr=subprocess.STDOUT)


def main():
    if len(sys.argv) != 11 or sys.argv[10] not in (
        "mono", "stereo", "surround51", "surround71", "mono-44100", "stereo-44100",
        "mono-48000", "stereo-48000", "mono-96000", "stereo-96000",
        "latency", "include-reload", "source-replug",
        "source-replug-stable-id",
        "server-restart", "plugin-live-param", "lv2-live-param",
        "vst3-live-param", "vst2-live-param", "plugin-bypass",
        "vst3-bypass", "lv2-bypass",
        "vst3-latency-change", "vst2-latency-change", "clap-latency-change",
        "lv2-latency-change", "lv2-pdc-final", "lv2-pdc-recovery",
        "lv2-pdc-transition-fault", "renegotiate",
        "transition-format", "device-filter", "stage-warning", "systemd-crash-restart",
        "device-switch", "delay-pdc", "source-consumer-latency"
    ):
        raise RuntimeError(
            "usage: pipewire_e2e_test.py PIPEWIRE PW_CLI PW_DUMP DAEMON CLI "
            "SOURCE CONSUMER PIPEWIRE_CONFIG DSP_CONFIG "
            "mono|stereo|surround51|surround71|mono-44100|stereo-44100|mono-48000|stereo-48000|"
            "mono-96000|stereo-96000|latency|include-reload|source-replug|"
            "source-replug-stable-id|"
            "plugin-live-param|lv2-live-param|vst3-live-param|"
            "vst2-live-param|plugin-bypass|vst3-bypass|lv2-bypass|vst3-latency-change|"
            "vst2-latency-change|"
            "clap-latency-change|lv2-latency-change|lv2-pdc-final|"
            "lv2-pdc-recovery|lv2-pdc-transition-fault|"
            "renegotiate|transition-format|systemd-crash-restart|"
            "device-filter|stage-warning|device-switch|delay-pdc|"
            "source-consumer-latency")
    (pipewire, pw_cli, pw_dump, daemon, cli, source, consumer, pw_config,
     dsp_config) = map(pathlib.Path, sys.argv[1:10])
    mode = sys.argv[10]
    surround51 = mode == "surround51"
    surround71 = mode == "surround71"
    rate_case = re.fullmatch(r"(mono|stereo)-(44100|48000|96000)", mode)
    mono = mode == "mono" or (rate_case is not None and
                               rate_case.group(1) == "mono")
    sample_rate = int(rate_case.group(2)) if rate_case else 48000
    latency_plugin = mode == "latency"
    include_reload = mode == "include-reload"
    stable_id_replug = mode == "source-replug-stable-id"
    source_replug = mode == "source-replug" or stable_id_replug
    server_restart = mode == "server-restart"
    vst2_live = mode == "vst2-live-param"
    vst2_latency_change = mode == "vst2-latency-change"
    vst3_latency_change = mode == "vst3-latency-change"
    clap_latency_change = mode == "clap-latency-change"
    lv2_latency_change = mode == "lv2-latency-change"
    lv2_pdc_final = mode == "lv2-pdc-final"
    lv2_pdc_recovery = mode == "lv2-pdc-recovery"
    lv2_pdc_transition_fault = mode == "lv2-pdc-transition-fault"
    delay_pdc = mode == "delay-pdc"
    plugin_live = mode in (
        "plugin-live-param", "lv2-live-param", "vst3-live-param") or vst2_live
    plugin_bypass = mode in ("plugin-bypass", "vst3-bypass", "lv2-bypass")
    renegotiate = mode == "renegotiate"
    transition_format = mode == "transition-format"
    device_filter = mode == "device-filter"
    stage_warning = mode == "stage-warning"
    device_switch = mode == "device-switch"
    systemd_crash_restart = mode == "systemd-crash-restart"
    latency_probe = mode == "source-consumer-latency"
    plugin_chain = (plugin_live or plugin_bypass or latency_plugin or
                    vst3_latency_change or vst2_latency_change or
                    clap_latency_change)
    plugin_chain = plugin_chain or lv2_latency_change
    plugin_chain = plugin_chain or lv2_pdc_final
    plugin_chain = plugin_chain or lv2_pdc_recovery or lv2_pdc_transition_fault
    if mode == "lv2-live-param":
        live_plugin_id, live_parameter = (
            "https://skyapo.example/plugins/test-gain", "gain")
    elif mode == "vst3-live-param":
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
                lv2_pdc_transition_fault):
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
        include_directory = config_home / "includes"
        include_child = include_directory / "include-reload-child.txt"
        if include_reload:
            include_directory.mkdir()
            included_config.write_text(
                "Include: includes/include-reload-child.txt\n")
            include_child.write_text("Preamp: -6 dB\n")
            dsp_config = included_config
        if transition_format:
            transition_config.write_text("Preamp: -6 dB\n")
            dsp_config = transition_config
        if lv2_pdc_transition_fault:
            pdc_recovery_config.write_text("Preamp: -120 dB\n")
            dsp_config = pdc_recovery_config
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
                "Stage: post-mix\nPreamp: 48 dB\n"
                "Stage: capture\nPreamp: -6 dB\n")
            dsp_config = staged_config
        if lv2_pdc_recovery:
            rejected_dynamic_pdc = (
                "Channel: L\n"
                "Plugin: LV2 https://skyapo.example/plugins/test-latency-increase\n"
                "Channel: L R\n"
                "Copy: L2=L" + "+R" * 430 + "\n")
            pdc_recovery_config.write_text(rejected_dynamic_pdc)
            dsp_config = pdc_recovery_config
        elif lv2_pdc_transition_fault:
            rejected_dynamic_pdc = (
                "Channel: L\n"
                "Plugin: LV2 https://skyapo.example/plugins/test-latency-increase\n"
                "Channel: L R\n"
                "Copy: L2=L" + "+R" * 430 + "\n")
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
        })
        if transition_format or lv2_pdc_transition_fault:
            env["SKYAPO_TEST_TRANSITION_MS"] = "500"
        logs = (root / "pipewire.log").open("w+")
        source_log = (root / "source.log").open("w+")
        daemon_log = (root / "daemon.log").open("w+")
        server = source_process = second_source_process = daemon_process = None
        startup_fault_consumer = None
        try:
            server = start_private_pipewire(
                pipewire, rate_config, env, logs, runtime)

            source_process = start_test_source(
                source, mono, env, source_log,
                serial="SKYAPO-TEST-SERIAL-42" if stable_id_replug else None,
                surround51=surround51, surround71=surround71,
                latency_probe=latency_probe)
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
                pipewire_objects = json.loads(
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

            # Observe the virtual source from its first available samples. The
            # latency-fault test must catch audio emitted by the very callback
            # that reports the changed plugin latency, not only later silence.
            if lv2_pdc_recovery:
                startup_fault_consumer = subprocess.Popen(
                    [str(consumer), "--expect-silent",
                     "--expected-rate", str(sample_rate)],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)

            if systemd_crash_restart:
                systemd_run = shutil.which("systemd-run")
                systemctl = shutil.which("systemctl")
                if not systemd_run or not systemctl:
                    raise RuntimeError(
                        "systemd-crash-restart requires systemd-run and systemctl")
                daemon_process = start_systemd_daemon(
                    systemd_run, systemctl, daemon, dsp_config, env,
                    os.environ.get("SKYAPO_SYSTEMD_UNIT_TEMPLATE", ""))
            else:
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
                        f"Sample rate: {sample_rate} Hz" in status and
                        (plugin_chain or delay_pdc or
                         "DSP amplitude ratio: 0.501187" in status)):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"daemon did not reach streaming state:\n{status}")
            expected_filters = (4 if (delay_pdc or lv2_pdc_recovery) else
                                (1 if lv2_pdc_transition_fault else
                                (1 if vst2_live else
                                (3 if lv2_pdc_final else
                                 (2 if plugin_chain else 1)))))
            expected_status = [
                f"Channels: {channel_count}", f"Filters: {expected_filters}",
                f"Channel positions: {channel_positions}",
                f"Active capture links: {channel_count}/{channel_count}",
                "Format: F32 planar DSP", f"Sample rate: {sample_rate} Hz",
                "Quantum: 1024",
                "Callback allocations: 0", "Callback deallocations: 0",
                "Default render endpoint: unavailable"]
            if stage_warning:
                expected_status.append(
                    "Config warnings:\n  " + str(dsp_config) + ":1: "
                    "pre-mix/post-mix processing is unavailable")
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
            if (not plugin_chain and not delay_pdc and
                    "DSP amplitude ratio: 0.501187" not in status):
                raise RuntimeError(
                    f"unexpected DSP amplitude ratio:\n{status}")
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

            if lv2_pdc_transition_fault:
                # Keep one independent reader connected from before reload,
                # throughout the 500 ms crossfade and fault handling. The old
                # graph is effectively silent; any candidate audio emitted
                # with an unvalidated PDC plan makes this measurement fail.
                transition_consumer = subprocess.Popen(
                    [str(consumer), "--expect-silent",
                     "--expected-rate", str(sample_rate)],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                try:
                    time.sleep(0.15)
                    pdc_recovery_config.write_text(rejected_dynamic_pdc)
                    reload_result = run([str(cli), "config", "reload"], env,
                                        timeout=10, check=False)
                    if (reload_result.returncode != 0 or
                            "Config reload succeeded" not in
                            reload_result.stdout):
                        raise RuntimeError(
                            "unsafe candidate was not staged for the PDC "
                            f"crossfade test:\n{reload_result.stdout}"
                            f"{reload_result.stderr}")
                    deadline = time.monotonic() + 8
                    transition_fault_status = ""
                    while time.monotonic() < deadline:
                        result = run([str(cli), "status"], env, timeout=5,
                                     check=False)
                        transition_fault_status = result.stdout
                        if (result.returncode == 0 and
                                "DSP state: paused (plugin latency refresh failed; "
                                "output silence)" in transition_fault_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "excessive plugin latency during crossfade did not "
                            "leave the runtime fail-closed:\n"
                            f"{transition_fault_status}")
                    for allocation_status in (
                            "Callback allocations: 0",
                            "Callback deallocations: 0"):
                        if allocation_status not in transition_fault_status:
                            raise RuntimeError(
                                "PDC crossfade fault handling violated the "
                                "realtime allocation invariant:\n"
                                f"{transition_fault_status}")
                    transition_stdout, transition_stderr = (
                        transition_consumer.communicate(timeout=8))
                    if transition_consumer.returncode:
                        raise RuntimeError(
                            "audio escaped during PDC crossfade fault:\n"
                            f"{transition_stdout}{transition_stderr}")
                    transition_silence = re.search(
                        r"RMS: ([0-9.]+)", transition_stdout)
                    if (not transition_silence or
                            float(transition_silence.group(1)) >= 1.0e-6):
                        raise RuntimeError(
                            "unvalidated PDC candidate emitted audio during "
                            "crossfade:\n"
                            f"{transition_stdout}{transition_stderr}")
                finally:
                    if transition_consumer.poll() is None:
                        transition_consumer.terminate()
                        transition_consumer.wait(timeout=3)

                pdc_recovery_config.write_text("Preamp: -6 dB\n")
                recovered = run([str(cli), "config", "reload"], env,
                                timeout=10, check=False)
                if (recovered.returncode != 0 or
                        "Config reload succeeded" not in recovered.stdout):
                    raise RuntimeError(
                        "valid replacement did not recover from the PDC "
                        f"crossfade fault:\n{recovered.stdout}{recovered.stderr}")
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
                        "safe config did not resume audio after the PDC "
                        f"crossfade fault:\n{recovered_status}")
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
                print("Excessive dynamic latency during a 500 ms config "
                      "crossfade remained silent; safe reload recovered to "
                      f"-6 dB audio (ratio {float(recovered_ratio.group(1)):.6f}).")

            if lv2_pdc_recovery:
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
                startup_stdout, startup_stderr = (
                    startup_fault_consumer.communicate(timeout=8))
                if startup_fault_consumer.returncode:
                    raise RuntimeError(
                        "audio escaped before the first PDC fault was latched; "
                        "the consumer was connected before daemon startup:\n"
                        f"{startup_stdout}{startup_stderr}")
                startup_silence = re.search(r"RMS: ([0-9.]+)", startup_stdout)
                if (not startup_silence or
                        float(startup_silence.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "the callback that detected changed plugin latency "
                        "published audio before fail-closed silence:\n"
                        f"{startup_stdout}{startup_stderr}")
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

                pdc_recovery_config.write_text("UnsupportedPdcRecovery: true\n")
                rejected = run([str(cli), "config", "reload"], env,
                               timeout=10, check=False)
                if ("Config reload failed; keeping last valid graph" not in
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

                # Recreating the same plugin config is not by itself a safe
                # recovery boundary: this test plugin reports a harmless
                # initial latency, then an excessive dynamic latency on its
                # first non-empty run. The replacement must be exercised and
                # have its PDC schedule validated before the daemon unmutes.
                pdc_recovery_config.write_text(rejected_dynamic_pdc)
                unsafe_recovery_consumer = subprocess.Popen(
                    [str(consumer), "--expect-silent",
                     "--expected-rate", str(sample_rate)],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                try:
                    time.sleep(0.15)
                    unsafe_recovery = run(
                        [str(cli), "config", "reload"], env,
                        timeout=10, check=False)
                    if ("Config reload failed; keeping last valid graph" not in
                            unsafe_recovery.stdout + unsafe_recovery.stderr):
                        raise RuntimeError(
                            "same dynamically unsafe plugin config was accepted "
                            "as PDC fault recovery:\n"
                            f"{unsafe_recovery.stdout}{unsafe_recovery.stderr}")
                    unsafe_status = run([str(cli), "status"], env,
                                        timeout=5, check=False)
                    if (unsafe_status.returncode != 0 or
                            "DSP state: paused (plugin latency refresh failed; "
                            "output silence)" not in unsafe_status.stdout):
                        raise RuntimeError(
                            "unvalidated replacement engine cleared the PDC "
                            f"fault latch:\n{unsafe_status.stdout}")
                    unsafe_stdout, unsafe_stderr = (
                        unsafe_recovery_consumer.communicate(timeout=8))
                    if unsafe_recovery_consumer.returncode:
                        raise RuntimeError(
                            "continuous consumer failed during rejected unsafe "
                            f"recovery:\n{unsafe_stdout}{unsafe_stderr}")
                finally:
                    if unsafe_recovery_consumer.poll() is None:
                        unsafe_recovery_consumer.terminate()
                        unsafe_recovery_consumer.wait(timeout=3)
                unsafe_silence = re.search(r"RMS: ([0-9.]+)", unsafe_stdout)
                if (not unsafe_silence or
                        float(unsafe_silence.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "unsafe plugin recovery emitted audio before being "
                        "rejected:\n"
                        f"{unsafe_stdout}{unsafe_stderr}")

                metadata = (os.environ.get("SKYAPO_PW_METADATA") or
                            shutil.which("pw-metadata"))
                if not metadata:
                    raise RuntimeError(
                        "pw-metadata is required to test faulted format rebuild")
                # Keep one independent consumer recording throughout the
                # format transition. A second capture after the transition
                # would miss a brief unmute/re-fault window.
                continuous_consumer = subprocess.Popen(
                    [str(consumer), "--expect-silent",
                     "--expected-rate", str(sample_rate)],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE)
                try:
                    time.sleep(0.15)
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
                                "Quantum: 512" in rebuilt_fault_status and
                                "DSP engine format: 48000 Hz, max block 512"
                                in rebuilt_fault_status and
                                "DSP state: paused (plugin latency refresh failed; "
                                "output silence)" in rebuilt_fault_status):
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(
                            "PipeWire quantum change resumed or lost the "
                            f"faulted DSP state:\n{rebuilt_fault_status}")
                    continuous_stdout, continuous_stderr = (
                        continuous_consumer.communicate(timeout=8))
                    if continuous_consumer.returncode:
                        raise RuntimeError(
                            "continuous consumer failed across the faulted "
                            f"format change:\n{continuous_stdout}"
                            f"{continuous_stderr}")
                finally:
                    if continuous_consumer.poll() is None:
                        continuous_consumer.terminate()
                        continuous_consumer.wait(timeout=3)
                silence_rms = re.search(r"RMS: ([0-9.]+)", continuous_stdout)
                if (not silence_rms or float(silence_rms.group(1)) >= 1.0e-6):
                    raise RuntimeError(
                        "faulted PipeWire format transition emitted non-silent "
                        "audio during the continuous recording:\n"
                        f"{continuous_stdout}{continuous_stderr}")

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
                            "Plugin latency update error:" not in
                            recovered_status):
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
                    r"RMS ratio to expected: ([0-9.]+)",
                    recovered_audio.stdout)
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
                daemon_process.send_signal(signal.SIGKILL)
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
                            restarts > restart_count and result.returncode == 0 and
                            "Daemon: streaming" in restarted_status and
                            "Active capture links: " +
                            f"{channel_count}/{channel_count}" in
                            restarted_status):
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
                print(f"User-systemd crash recovery passed: skyapod PID "
                      f"{old_pid} -> {new_pid}; restart count "
                      f"{daemon_process.restart_count()}; virtual source "
                      f"node {old_virtual_id} -> {new_virtual_id}.")

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

            if lv2_pdc_final:
                # Keep the input graph steady while measuring channel phase:
                # the purpose of this case is physical capture through the
                # mono-latency plugin and PDC into an independent PipeWire
                # consumer, not link-removal recovery.
                link_id = "not removed (PDC measurement)"
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
                print("Stopping selected deterministic source…", flush=True)
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
                        pipewire, pw_config, env, logs, runtime)

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
                            "skyapod exited while recovering selected source")
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

            expected_db = (0.0 if (lv2_pdc_final or delay_pdc) else
                           (-12.020599913 if
                            (latency_plugin or clap_latency_change or
                             vst2_latency_change) else
                            (-3.0 if transition_format else -6.0)))
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
                    [str(cli), "config", "reload"], env, timeout=8)
                if ("Config reload failed; keeping last valid graph" not in
                        burst_rejected.stdout):
                    raise RuntimeError(
                        "invalid reload burst did not retain last graph:\n"
                        f"{burst_rejected.stdout}")
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
                      "directory removal retained it; directory recreation "
                      "reloaded to -2 dB. Five successive live transitions, "
                      "an invalid reload, a valid recovery, and a newly "
                      "created missing Include recovery also passed.")
                expected_db = -4.0

            consumer_args = [str(consumer)]
            if mono:
                consumer_args.append("--mono")
            elif surround51:
                consumer_args.append("--surround51")
            elif surround71:
                consumer_args.append("--surround71")
            consumer_args += ["--expected-rate", str(sample_rate)]
            if latency_probe:
                consumer_args += ["--accept-any-audio", "--measure-source-lag"]
            if lv2_pdc_final or delay_pdc:
                consumer_args += ["--expect-identical-channels"]
            if plugin_chain:
                consumer_args += ["--expected-db", str(expected_db)]
            elif include_reload or transition_format or delay_pdc:
                consumer_args += ["--expected-db", str(expected_db)]
            consumer_graph = ""
            consumer_graph_summary = ""
            try:
                if lv2_pdc_final:
                    consumer_process = subprocess.Popen(
                        consumer_args, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    time.sleep(0.2)
                    consumer_graph = run(
                        [str(pw_dump)], env, timeout=5,
                        check=False).stdout
                    consumer_graph_summary = summarize_graph_dump(
                        consumer_graph)
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
                if not lv2_pdc_final:
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
            if (not latency_probe and
                    (not match or abs(float(match.group(1)) - 1.0) >= 0.03)):
                raise RuntimeError(
                    f"capture output did not report expected "
                    f"{expected_db:g} dB:\n"
                    f"{captured.stdout}")
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
            if mode == "plugin-live-param":
                state_dir = state_home / "skyapo" / "clap-state"
                sidecars = list(state_dir.glob("*.clapstate"))
                if len(sidecars) != 1:
                    raise RuntimeError(
                        "orderly CLAP daemon stop did not save exactly one "
                        f"state sidecar: {sidecars}")
                if sidecars[0].stat().st_mode & 0o077:
                    raise RuntimeError("CLAP state sidecar permissions are not 0600")
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
                            "Active capture links: 2/2" in restored_status):
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError(
                        "daemon did not resume the CLAP state graph:\n"
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
                        "fresh daemon did not restore saved CLAP audio state:\n"
                        f"{restored_capture.stdout}")
                valid_state = sidecars[0].read_bytes()
                corrupted_state = bytearray(valid_state)
                corrupted_state[-1] ^= 0x01
                sidecars[0].write_bytes(corrupted_state)
                rejected_reload = run(
                    [str(cli), "config", "reload"], env, check=False)
                if ("Config reload failed; keeping last valid graph" not in
                        rejected_reload.stdout or
                        "checksum mismatch" not in rejected_reload.stdout):
                    raise RuntimeError(
                        "runtime accepted a corrupt CLAP state sidecar:\n"
                        f"{rejected_reload.stdout}")
                retained_status = run([str(cli), "status"], env).stdout
                if "Daemon: streaming" not in retained_status:
                    raise RuntimeError(
                        "corrupt CLAP state stopped the active graph:\n"
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
                        "corrupt state did not retain the prior live DSP "
                        f"output:\n{retained_capture.stdout}")
                sidecars[0].write_bytes(valid_state)
                recovered_reload = run(
                    [str(cli), "config", "reload"], env, check=False)
                if "Config reload succeeded" not in recovered_reload.stdout:
                    raise RuntimeError(
                        "restored valid sidecar could not reload:\n"
                        f"{recovered_reload.stdout}")
                stopped_again = run([str(cli), "stop"], env, timeout=8)
                if "Stopping skyapod" not in stopped_again.stdout:
                    raise RuntimeError(
                        "restored daemon stop was not acknowledged")
                daemon_process.wait(timeout=5)
                if daemon_process.returncode != 0:
                    raise RuntimeError(
                        "restored skyapod exited with status "
                        f"{daemon_process.returncode}")
                print("CLAP state survived orderly shutdown/recreation; "
                      f"independent capture: {restored_capture.stdout.strip()}")
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
                          (startup_fault_consumer, "startup PDC observer"),
                          (source_process, "test source"),
                          (second_source_process, "alternate test source"),
                          (server, "PipeWire server")))
            except Exception as cleanup_error:
                print(f"E2E cleanup warning: {cleanup_error}", file=sys.stderr)
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
