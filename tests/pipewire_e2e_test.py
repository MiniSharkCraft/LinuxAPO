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


def start_test_source(source, mono, env, source_log):
    return subprocess.Popen(
        [str(source)] + (["--mono"] if mono else []), env=env,
        stdout=source_log, stderr=subprocess.STDOUT)


def main():
    if len(sys.argv) != 11 or sys.argv[10] not in (
        "mono", "stereo", "mono-44100", "stereo-44100",
        "mono-48000", "stereo-48000", "mono-96000", "stereo-96000",
        "latency", "include-reload", "source-replug",
        "server-restart", "plugin-live-param", "lv2-live-param",
        "vst3-live-param", "vst2-live-param", "plugin-bypass",
        "renegotiate", "transition-format"
    ):
        raise RuntimeError(
            "usage: pipewire_e2e_test.py PIPEWIRE PW_CLI PW_DUMP DAEMON CLI "
            "SOURCE CONSUMER PIPEWIRE_CONFIG DSP_CONFIG "
            "mono|stereo|mono-44100|stereo-44100|mono-48000|stereo-48000|"
            "mono-96000|stereo-96000|latency|include-reload|source-replug|"
            "plugin-live-param|lv2-live-param|vst3-live-param|"
            "vst2-live-param|plugin-bypass|renegotiate|transition-format")
    (pipewire, pw_cli, pw_dump, daemon, cli, source, consumer, pw_config,
     dsp_config) = map(pathlib.Path, sys.argv[1:10])
    mode = sys.argv[10]
    rate_case = re.fullmatch(r"(mono|stereo)-(44100|48000|96000)", mode)
    mono = mode == "mono" or (rate_case is not None and
                               rate_case.group(1) == "mono")
    sample_rate = int(rate_case.group(2)) if rate_case else 48000
    latency_plugin = mode == "latency"
    include_reload = mode == "include-reload"
    source_replug = mode == "source-replug"
    server_restart = mode == "server-restart"
    vst2_live = mode == "vst2-live-param"
    plugin_live = mode in (
        "plugin-live-param", "lv2-live-param", "vst3-live-param") or vst2_live
    plugin_bypass = mode == "plugin-bypass"
    renegotiate = mode == "renegotiate"
    transition_format = mode == "transition-format"
    plugin_chain = plugin_live or plugin_bypass or latency_plugin
    if mode == "lv2-live-param":
        live_plugin_id, live_parameter = (
            "https://skyapo.example/plugins/test-gain", "gain")
    elif mode == "vst3-live-param":
        live_plugin_id, live_parameter = (
            "534B5941504F00010000000000000001", "7")
    elif vst2_live:
        live_plugin_id = os.environ["SKYAPO_TEST_VST2_PATH"]
        live_parameter = "Gain"
    else:
        live_plugin_id, live_parameter = "org.skyapo.test.gain", "Gain"
    device_name = "skyapo.test.mono" if mono else "skyapo.test.input"
    channel_count = 1 if mono else 2
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
        if renegotiate or transition_format:
            text = rate_config.read_text()
            text = text.replace(
                "default.clock.allowed-rates = [ 48000 ]",
                "default.clock.allowed-rates = [ 44100 48000 96000 ]")
            if "default.clock.allowed-rates = [ 44100 48000 96000 ]" not in text:
                raise RuntimeError("could not enable runtime test sample rates")
            rate_config.write_text(text)
        included_config = config_home / "include-reload-root.txt"
        transition_config = config_home / "transition-format.txt"
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
        env = os.environ.copy()
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
        })
        if transition_format:
            env["SKYAPO_TEST_TRANSITION_MS"] = "500"
        logs = (root / "pipewire.log").open("w+")
        source_log = (root / "source.log").open("w+")
        daemon_log = (root / "daemon.log").open("w+")
        server = source_process = daemon_process = None
        try:
            server = start_private_pipewire(
                pipewire, rate_config, env, logs, runtime)

            source_process = start_test_source(source, mono, env, source_log)
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
            current = run([str(cli), "device", "current"], env)
            if device_name not in current.stdout:
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
                        f"Sample rate: {sample_rate} Hz" in status):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"daemon did not reach streaming state:\n{status}")
            expected_filters = 1 if vst2_live else (2 if plugin_chain else 1)
            channel_positions = "MONO" if mono else "FL FR"
            expected_status = [
                f"Channels: {channel_count}", f"Filters: {expected_filters}",
                f"Channel positions: {channel_positions}",
                f"Active capture links: {channel_count}/{channel_count}",
                "Format: F32 planar DSP", f"Sample rate: {sample_rate} Hz",
                "Quantum: 1024",
                "Callback allocations: 0", "Callback deallocations: 0",
                "Default render endpoint: unavailable"]
            if latency_plugin:
                expected_status.append(
                    "Plugin-reported latency sum: 64 samples "
                    "(no delay compensation)")
            for expected in expected_status:
                if expected not in status:
                    raise RuntimeError(f"missing runtime value {expected!r}:\n{status}")
            if (not plugin_chain and
                    "DSP amplitude ratio: 0.501187" not in status):
                raise RuntimeError(
                    f"unexpected DSP amplitude ratio:\n{status}")

            old_virtual_id = virtual_source_id(status)
            assert_virtual_source(pw_dump, env, old_virtual_id)

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

                print("Daemon detected removal; publishing the same stable "
                      "source name again…", flush=True)
                source_process = start_test_source(
                    source, mono, env, source_log)
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
                print(f"Selected source/server recovery ({mode}) returned "
                      f"SkyAPO Virtual Mic at node {new_virtual_id} "
                      f"(previous graph ID {old_virtual_id}; IDs may be reused "
                      f"after a server restart) with {channel_count}/"
                      f"{channel_count} capture links.")
                recovered_status = relinked_status

            expected_db = (-12.020599913 if latency_plugin else
                           (-3.0 if transition_format else -6.0))
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
                               "org.skyapo.test.gain", "on"], env)
                if changed.stdout != "Bypassed org.skyapo.test.gain\n":
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
                print("Include edit reloaded to -3 dB; invalid edit and "
                      "directory removal retained it; directory recreation "
                      "reloaded to -2 dB. Five successive live transitions, "
                      "an invalid reload, and a valid recovery also passed.")
                expected_db = -2.0

            consumer_args = [str(consumer)] + (["--mono"] if mono else [])
            consumer_args += ["--expected-rate", str(sample_rate)]
            if plugin_chain:
                consumer_args += ["--expected-db", str(expected_db)]
            elif include_reload or transition_format:
                consumer_args += ["--expected-db", str(expected_db)]
            captured = run(consumer_args, env, timeout=12)
            if f"Sample rate: {sample_rate} Hz" not in captured.stdout:
                raise RuntimeError(
                    f"independent consumer did not negotiate {sample_rate} Hz:\n"
                    f"{captured.stdout}")
            match = re.search(r"RMS ratio to expected: ([0-9.]+)",
                              captured.stdout)
            if not match or abs(float(match.group(1)) - 1.0) >= 0.03:
                raise RuntimeError(
                    f"capture output did not report expected "
                    f"{expected_db:g} dB:\n"
                    f"{captured.stdout}")
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
                          (source_process, "test source"),
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
