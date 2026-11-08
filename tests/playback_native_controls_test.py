#!/usr/bin/env python3
"""Compare playback controls with a native adapter in an isolated graph."""

import argparse
import json
import math
import os
import pathlib
import subprocess
import tempfile
import time

from pipewire_e2e_test import (make_rate_config, start_private_pipewire,
                               wait_for_consumer_ready)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--pipewire-config", type=pathlib.Path, required=True)
    parser.add_argument("--pipewire", default="pipewire")
    parser.add_argument("--pw-cli", default="pw-cli")
    parser.add_argument("--pw-dump", default="pw-dump")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source = build / "skyapo-pipewire-test-source"
    consumer = build / "skyapo-pipewire-test-consumer"
    cli = build / "skyapo"
    daemon = build / "skyapod"
    config = pathlib.Path(__file__).resolve().parents[1] / "examples/preamp.txt"
    processes = []
    diagnostics = []
    latest_graph = []
    with tempfile.TemporaryDirectory(prefix="skyapo-native-controls-") as temp:
        root = pathlib.Path(temp)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = os.environ.copy()
        env.pop("PIPEWIRE_REMOTE", None)
        env.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "PIPEWIRE_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(root / "config"),
            "XDG_STATE_HOME": str(root / "state"),
            "GIO_USE_VFS": "local",
        })
        rate_config = make_rate_config(
            args.pipewire_config, root / "pipewire.conf", 48000)
        with (root / "processes.log").open("w+") as log:
            def run(command, timeout=8):
                result = subprocess.run(
                    [str(part) for part in command], env=env, text=True,
                    capture_output=True, timeout=timeout)
                if result.returncode:
                    raise RuntimeError(
                        f"command failed ({result.returncode}): {command}\n"
                        f"{result.stdout}\n{result.stderr}")
                return result.stdout

            def spawn(command, **kwargs):
                process = subprocess.Popen(
                    [str(part) for part in command], env=env,
                    stdout=log, stderr=log, **kwargs)
                processes.append(process)
                return process

            def graph():
                nonlocal latest_graph
                latest_graph = json.loads(run([args.pw_dump]))
                return latest_graph

            def nodes(objects):
                return {
                    item.get("info", {}).get("props", {}).get("node.name"):
                    item for item in objects
                    if item.get("type", "").endswith(":Node")}

            def wait_for(description, predicate, timeout=8):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if any(process.poll() is not None for process in processes):
                        raise RuntimeError(f"process exited while waiting for {description}")
                    value = predicate()
                    if value:
                        return value
                    time.sleep(0.05)
                raise RuntimeError(f"timed out waiting for {description}")

            def assert_chain(objects, with_producers):
                by_name = nodes(objects)
                required = [("skyapo.playback", "skyapo.playback.dsp"),
                            ("skyapo.playback.dsp", "skyapo.playback.stream"),
                            ("skyapo.playback.stream", "probe.target")]
                if with_producers:
                    required += [("probe.app", "skyapo.playback"),
                                 ("probe.reference-source", "probe.reference")]
                for name in ("skyapo.playback.dsp", "skyapo.playback.stream"):
                    media_class = by_name.get(name, {}).get("info", {}).get(
                        "props", {}).get("media.class", "")
                    if media_class.startswith(("Audio/", "Stream/")):
                        raise RuntimeError(f"internal filter exposed to adapter policy: {name}")
                links = [item for item in objects
                         if item.get("type", "").endswith(":Link")]
                for output, input_node in required:
                    output_id = by_name.get(output, {}).get("id")
                    input_id = by_name.get(input_node, {}).get("id")
                    active = [item for item in links
                              if item.get("info", {}).get("state") == "active"
                              and item["info"].get("output-node-id") == output_id
                              and item["info"].get("input-node-id") == input_id]
                    if output_id is None or input_id is None or len(active) != 2:
                        return False
                if not with_producers:
                    frontend_id = by_name["skyapo.playback"]["id"]
                    if any(item.get("info", {}).get("input-node-id") == frontend_id
                           for item in links):
                        raise RuntimeError("frontend has a producer before startup readiness check")
                return True

            def read_controls(name):
                item = nodes(graph())[name]
                return item["info"]["params"]["Props"][0]

            def controls_match(actual, expected):
                return (abs(actual["volume"] - expected["volume"]) < 1e-6
                        and actual["mute"] == expected["mute"]
                        and len(actual["channelVolumes"]) == 2
                        and all(abs(a - b) < 1e-6 for a, b in zip(
                            actual["channelVolumes"], expected["channelVolumes"])))

            try:
                processes.append(start_private_pipewire(
                    args.pipewire, rate_config, env, log, runtime))
                manager = spawn([args.pw_cli], stdin=subprocess.PIPE, text=True)
                for name, monitor_volumes in (("probe.target", False),
                                              ("probe.reference", True)):
                    manager.stdin.write(
                        'create-node adapter { factory.name = support.null-audio-sink '
                        f'node.name = "{name}" media.class = Audio/Sink '
                        'audio.channels = 2 audio.position = [ FL FR ] '
                        f'monitor.channel-volumes = {str(monitor_volumes).lower()} '
                        'adapter.auto-port-config = '
                        '{ mode = dsp monitor = true position = preserve } }\n')
                manager.stdin.flush()
                spawn([source])
                wait_for("capture source and native sinks", lambda:
                         {"skyapo.test.input", "probe.target", "probe.reference"}
                         <= nodes(graph()).keys())
                run([cli, "device", "set", "skyapo.test.input"])
                run([cli, "device", "output", "set", "probe.target"])
                spawn([daemon, "--config", config])

                def ready_without_producer():
                    result = subprocess.run(
                        [str(cli), "status"], env=env, text=True,
                        capture_output=True, timeout=8)
                    if (result.returncode == 0 and
                            "Playback routing policy: waiting for PipeWire default metadata"
                            in result.stdout and assert_chain(graph(), False)):
                        diagnostics.append(result.stdout)
                        return True
                    return False

                wait_for("playback readiness without an application", ready_without_producer)
                spawn([source, "--name", "probe.app", "--link-node", "skyapo.playback"])
                spawn([source, "--name", "probe.reference-source", "--link-node",
                       "probe.reference"])
                wait_for("both complete audio paths", lambda: assert_chain(graph(), True))
                frontend = nodes(latest_graph)["skyapo.playback"]
                reference = nodes(latest_graph)["probe.reference"]
                for item, expected_monitor_gain in ((frontend, False), (reference, True)):
                    controls = item["info"]["params"]["Props"][0]
                    native_params = controls["params"]
                    settings = dict(zip(native_params[::2], native_params[1::2]))
                    if settings.get("monitor.channel-volumes") != expected_monitor_gain:
                        raise RuntimeError("native adapter monitor gain configuration differs")
                sequence = [
                    ("baseline", {"volume": 1.0, "channelVolumes": [1.0, 1.0],
                                  "mute": False}),
                    ("scalar-half", {"volume": 0.5}),
                    ("channels-half", {"channelVolumes": [0.5, 0.5]}),
                    ("scalar-point-eight", {"volume": 0.8}),
                    ("same-channels-again", {"channelVolumes": [0.5, 0.5]}),
                    ("mute", {"mute": True}),
                    ("unmute", {"mute": False}),
                    ("nonuniform", {"channelVolumes": [0.5, 0.25]}),
                ]
                expected = {}
                for label, changes in sequence:
                    expected.update(changes)
                    for name, item in (("probe.reference", reference),
                                       ("skyapo.playback", frontend)):
                        run([args.pw_cli, "set-param", item["id"], "Props",
                             json.dumps(changes)])
                        wait_for(f"{label} controls on {name}", lambda name=name:
                                 controls_match(read_controls(name), expected))
                    gain = math.sqrt(sum(value * value for value in
                                         expected["channelVolumes"]) / 2)
                    for name, capture, preamp in (
                            ("probe.reference", "probe.reference", 0.0),
                            ("skyapo.playback", "skyapo.playback.dsp", -6.0)):
                        actual = read_controls(name)
                        diagnostics.append(f"{label} {name} controls: " + repr({
                            key: actual[key] for key in expected}))
                        command = [consumer, "--capture-node", capture,
                                   "--duration-ms", "1500"]
                        if expected["mute"]:
                            command += ["--expect-silent"]
                        else:
                            command += ["--expected-db", str(preamp + 20 * math.log10(gain)),
                                        "--ignore-silent-blocks"]
                        output = run(command)
                        diagnostics.append(f"{label} {name}:\n{output}")
                        print(f"{label}: {name} controls=" + repr({
                            key: actual[key] for key in expected}) +
                            f"; native gain={gain:.9f}\n{output}", flush=True)
                    if not assert_chain(graph(), True):
                        raise RuntimeError(f"audio path changed during {label}")
                # Keep a recorder on the stable DSP output while recreating
                # only our public adapter. A muted, nonuniform gain must not
                # briefly revert to the new adapter's unmuted unity defaults.
                expected["mute"] = True
                run([args.pw_cli, "set-param", frontend["id"], "Props",
                     json.dumps({"mute": True})])
                wait_for("mute before frontend removal", lambda:
                         controls_match(read_controls("skyapo.playback"), expected))
                stable_dsp = nodes(graph())["skyapo.playback.dsp"]["id"]
                recorder = subprocess.Popen(
                    [str(consumer), "--capture-node", "skyapo.playback.dsp",
                     "--name", "probe.lifecycle-recorder", "--expect-silent",
                     "--duration-ms", "4000", "--signal-ready"],
                    env=env, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT)
                processes.append(recorder)
                ready_output = wait_for_consumer_ready(
                    recorder, "muted frontend recreation recorder")
                old_frontend = frontend["id"]
                old_frontend_serial = frontend["info"]["props"]["object.serial"]
                run([args.pw_cli, "destroy", old_frontend])

                def recreated_frontend():
                    item = nodes(graph()).get("skyapo.playback")
                    return (item if item and item["info"]["props"]["object.serial"]
                            != old_frontend_serial else None)

                frontend = wait_for("owned frontend recreation", recreated_frontend)
                wait_for("restored nonuniform gain and mute", lambda:
                         controls_match(read_controls("skyapo.playback"), expected))
                objects = graph()
                if nodes(objects)["skyapo.playback.dsp"]["id"] != stable_dsp:
                    raise RuntimeError("frontend recreation replaced the stable DSP node")
                app_id = nodes(objects)["probe.app"]["id"]
                # The deterministic fixture owns its first links and does not
                # reconnect removed targets. Reattach its real surviving output
                # ports to the recreated adapter while the recorder stays armed.
                for channel in ("FL", "FR"):
                    def find_port(node_id, direction):
                        return next(item["id"] for item in objects
                                    if item.get("type", "").endswith(":Port")
                                    and item.get("info", {}).get("props", {}).get("node.id") == node_id
                                    and item["info"]["props"].get("port.direction") == direction
                                    and item["info"]["props"].get("audio.channel") == channel)
                    output_port = find_port(app_id, "out")
                    input_port = find_port(frontend["id"], "in")
                    manager.stdin.write(
                        f"create-link {app_id} {output_port} {frontend['id']} {input_port}\n")
                manager.stdin.flush()
                wait_for("recreated complete signal path", lambda:
                         assert_chain(graph(), True))
                remaining, _ = recorder.communicate(timeout=8)
                recorder_output = ready_output + remaining
                diagnostics.append("Muted recreation recorder:\n" + recorder_output)
                if recorder.returncode:
                    raise RuntimeError("frontend recreation leaked audio while muted:\n"
                                       + recorder_output)
                processes.remove(recorder)
                print(f"Frontend recreated {old_frontend} -> {frontend['id']} "
                      f"(serial {old_frontend_serial} -> "
                      f"{frontend['info']['props']['object.serial']}); "
                      f"stable DSP {stable_dsp}; restored controls={expected}\n"
                      + recorder_output, flush=True)
                expected["mute"] = False
                run([args.pw_cli, "set-param", frontend["id"], "Props",
                     json.dumps({"mute": False})])
                wait_for("unmute after recreation", lambda:
                         controls_match(read_controls("skyapo.playback"), expected))
                restored_audio = run([
                    consumer, "--capture-node", "skyapo.playback.dsp",
                    "--duration-ms", "1500", "--ignore-silent-blocks",
                    "--expected-db", str(-6 + 20 * math.log10(gain))])
                diagnostics.append("Restored nonuniform gain:\n" + restored_audio)
                print("Restored nonuniform gain:\n" + restored_audio, flush=True)
                if not assert_chain(graph(), True):
                    raise RuntimeError("signal path changed after frontend recreation")
                final_status = run([cli, "status"])
                diagnostics.append(final_status)
                for counter in ("Callback allocations: 0\n", "Callback deallocations: 0\n"):
                    if counter not in final_status:
                        raise RuntimeError(f"realtime executable allocation audit failed: {counter.strip()}")
                print(final_status)
                print("Native reference and post-DSP playback controls matched in all 8 cases.")
            except Exception:
                log.flush()
                log.seek(0)
                print("\n".join(diagnostics))
                print("Latest graph:\n" + json.dumps(latest_graph, indent=2))
                print("Processes:\n" + log.read())
                raise
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)


if __name__ == "__main__":
    main()
