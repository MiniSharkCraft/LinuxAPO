#!/usr/bin/env python3
"""Exercise skyapo-bench against generated actual EAPO Preamp chains."""

import json
import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave


def main():
    if len(sys.argv) not in (2, 3) or (
        len(sys.argv) == 3 and sys.argv[2] != "--convolution"
    ):
        raise RuntimeError("usage: benchmark_test.py SKYAPO_BENCH [--convolution]")
    benchmark = pathlib.Path(sys.argv[1])
    convolution = len(sys.argv) == 3
    with tempfile.TemporaryDirectory(prefix="skyapo-bench-test-") as temp:
        root = pathlib.Path(temp)
        if convolution:
            # Use PCM16 so the fixture needs only Python's standard library.
            # The measured chain is still upstream Convolution/libHybridConv.
            convolution_measurements = []
            for sample_rate in (44100, 48000, 96000, 192000):
                for tap_count in (3, 257, 4097):
                    ir_path = root / f"impulse-{sample_rate}-{tap_count}.wav"
                    with wave.open(str(ir_path), "wb") as impulse:
                        impulse.setnchannels(1)
                        impulse.setsampwidth(2)
                        impulse.setframerate(sample_rate)
                        samples = bytearray()
                        for tap in range(tap_count):
                            value = int(12000 / (tap + 1))
                            samples.extend(struct.pack("<h", value))
                        impulse.writeframes(samples)
                    config = root / f"convolution-{sample_rate}-{tap_count}.txt"
                    config.write_text(f'Convolution: "{ir_path}"\n')
                    result = subprocess.run(
                        [
                            str(benchmark), "--config", str(config),
                            "--rate", str(sample_rate), "--channels", "2",
                            "--block", "128", "--warmup", "5",
                            "--iterations", "40",
                        ],
                        text=True,
                        capture_output=True,
                        timeout=20,
                    )
                    if result.returncode:
                        raise RuntimeError(
                            "convolution benchmark failed for "
                            f"{tap_count} taps at {sample_rate} Hz:\n"
                            f"{result.stderr}"
                        )
                    measurement = json.loads(result.stdout)
                    if measurement["schema"] != "skyapo-bench-v3":
                        raise RuntimeError(
                            "unexpected convolution benchmark schema"
                        )
                    if measurement["filter_count"] != 1:
                        raise RuntimeError(
                            "expected one actual upstream Convolution filter"
                        )
                    for metric in (
                        "mean_us", "median_us", "p95_us", "max_us",
                        "block_budget_us", "max_rss_kib",
                        "p95_budget_percent",
                    ):
                        if (
                            not isinstance(measurement[metric], (float, int))
                            or not math.isfinite(measurement[metric])
                            or measurement[metric] < 0
                        ):
                            raise RuntimeError(
                                f"invalid convolution metric {metric}"
                            )
                    if measurement["max_rss_kib"] <= 0:
                        raise RuntimeError(
                            "convolution process peak RSS was not reported"
                        )
                    convolution_bytes = measurement[
                        "convolution_estimated_bytes"
                    ]
                    if not 0 < convolution_bytes <= 64 * 1024 * 1024:
                        raise RuntimeError(
                            "convolution estimate was not reported within "
                            "the documented per-graph budget"
                        )
                    if (
                        measurement["engine_audio_buffer_payload_bytes"]
                        < convolution_bytes
                    ):
                        raise RuntimeError(
                            "engine payload omitted the convolution memory "
                            "estimate"
                        )
                    if measurement["measured_blocks"] != 40:
                        raise RuntimeError(
                            "convolution measured block count was not recorded"
                        )
                    convolution_measurements.append(
                        (
                            sample_rate,
                            tap_count,
                            measurement["median_us"],
                            measurement["p95_us"],
                            measurement["block_budget_us"],
                            measurement["p95_budget_percent"],
                            measurement["max_rss_kib"],
                            convolution_bytes,
                        )
                    )
            print("Upstream Convolution benchmark (times in microseconds):")
            print("rate  taps  median  p95  budget  p95-budget%  process-RSS-KiB  "
                  "estimated-bytes")
            for result in convolution_measurements:
                rate, taps, median, p95, budget, percent, rss, estimate = result
                print(
                    f"{rate:5d} {taps:5d} {median:7.2f} {p95:7.2f} "
                    f"{budget:7.2f} {percent:12.2f} {rss:16d} {estimate:16d}"
                )
            return

        for count in (1, 4, 16):
            config = root / f"preamp-{count}.txt"
            config.write_text("Preamp: -0.01 dB\n" * count)
            result = subprocess.run(
                [
                    str(benchmark),
                    "--config", str(config),
                    "--rate", "48000",
                    "--channels", "2",
                    "--block", "128",
                    "--warmup", "5",
                    "--iterations", "40",
                ],
                text=True,
                capture_output=True,
                timeout=10,
            )
            if result.returncode:
                raise RuntimeError(f"benchmark failed:\n{result.stderr}")
            measurement = json.loads(result.stdout)
            if measurement["schema"] != "skyapo-bench-v3":
                raise RuntimeError("unexpected benchmark schema")
            if measurement["filter_count"] != count:
                raise RuntimeError(
                    f"expected {count} actual filters, got {measurement['filter_count']}"
                )
            for metric in (
                "mean_us", "median_us", "p95_us", "max_us",
                "block_budget_us", "max_rss_kib", "p95_budget_percent",
                "filter_configuration_scratch_bytes", "iir_filter_bytes",
                "convolution_estimated_bytes", "eapo_delay_ring_bytes",
                "pdc_ring_capacity_bytes",
                "plugin_bypass_ring_capacity_bytes",
                "engine_audio_buffer_payload_bytes",
            ):
                if (
                    not isinstance(measurement[metric], (float, int))
                    or not math.isfinite(measurement[metric])
                    or measurement[metric] < 0
                ):
                    raise RuntimeError(f"invalid benchmark metric {metric}")
            if measurement["max_rss_kib"] <= 0:
                raise RuntimeError("process peak RSS was not reported")
            expected_scratch = 2 * 2 * 128 * 4
            if (
                measurement["filter_configuration_scratch_bytes"]
                != expected_scratch
                or measurement["iir_filter_bytes"] != 0
                or measurement["convolution_estimated_bytes"] != 0
                or measurement["eapo_delay_ring_bytes"] != 0
                or measurement["pdc_ring_capacity_bytes"] != 0
                or measurement["plugin_bypass_ring_capacity_bytes"] != 0
                or measurement["engine_audio_buffer_payload_bytes"]
                != expected_scratch
            ):
                raise RuntimeError(
                    "known engine sample-buffer payload did not match the "
                    "2-channel, 128-frame FilterConfiguration scratch planes"
                )
            if measurement["measured_blocks"] != 40:
                raise RuntimeError("measured block count was not recorded")

        delay_config = root / "delay.txt"
        delay_config.write_text("Delay: 10 samples\n")
        result = subprocess.run(
            [
                str(benchmark), "--config", str(delay_config),
                "--rate", "48000", "--channels", "2", "--block", "128",
                "--warmup", "5", "--iterations", "40",
            ],
            text=True,
            capture_output=True,
            timeout=10,
        )
        if result.returncode:
            raise RuntimeError(f"delay benchmark failed:\n{result.stderr}")
        delay_measurement = json.loads(result.stdout)
        if (
            delay_measurement["schema"] != "skyapo-bench-v3"
            or delay_measurement["filter_count"] != 1
            or delay_measurement["iir_filter_bytes"] != 0
            or delay_measurement["convolution_estimated_bytes"] != 0
            or delay_measurement["eapo_delay_ring_bytes"] != 2 * 10 * 4
            or delay_measurement["pdc_ring_capacity_bytes"] != 0
            or delay_measurement["plugin_bypass_ring_capacity_bytes"] != 0
            or delay_measurement["engine_audio_buffer_payload_bytes"]
            != 2 * 2 * 128 * 4 + 2 * 10 * 4
        ):
            raise RuntimeError(
                "known engine sample-buffer payload did not include the "
                "upstream 10-sample stereo DelayFilter rings"
            )

        iir_config = root / "iir.txt"
        iir_config.write_text(
            "Filter: ON IIR Order 2 Coefficients 1 0.5 -0.25 2 -0.5 0.25\n"
        )
        result = subprocess.run(
            [
                str(benchmark), "--config", str(iir_config),
                "--rate", "48000", "--channels", "2", "--block", "128",
                "--warmup", "5", "--iterations", "40",
            ],
            text=True,
            capture_output=True,
            timeout=10,
        )
        if result.returncode:
            raise RuntimeError(f"IIR benchmark failed:\n{result.stderr}")
        iir_measurement = json.loads(result.stdout)
        if (
            iir_measurement["schema"] != "skyapo-bench-v3"
            or iir_measurement["filter_count"] != 1
            or iir_measurement["iir_filter_bytes"] != 96
            or iir_measurement["convolution_estimated_bytes"] != 0
            or iir_measurement["engine_audio_buffer_payload_bytes"]
            != 2 * 2 * 128 * 4 + 96
        ):
            raise RuntimeError(
                "known engine payload did not include the upstream "
                "second-order stereo IIR arrays"
            )
    print("skyapo-bench measured 1/4/16 actual upstream Preamp filters.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"benchmark test failed: {error}", file=sys.stderr)
        sys.exit(1)
