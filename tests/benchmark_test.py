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
            for tap_count in (3, 257, 4097):
                ir_path = root / f"impulse-{tap_count}.wav"
                with wave.open(str(ir_path), "wb") as impulse:
                    impulse.setnchannels(1)
                    impulse.setsampwidth(2)
                    impulse.setframerate(48000)
                    samples = bytearray()
                    for tap in range(tap_count):
                        value = int(12000 / (tap + 1))
                        samples.extend(struct.pack("<h", value))
                    impulse.writeframes(samples)
                config = root / f"convolution-{tap_count}.txt"
                config.write_text(f'Convolution: "{ir_path}"\n')
                result = subprocess.run(
                    [
                        str(benchmark), "--config", str(config),
                        "--rate", "48000", "--channels", "2", "--block", "128",
                        "--warmup", "5", "--iterations", "40",
                    ],
                    text=True,
                    capture_output=True,
                    timeout=20,
                )
                if result.returncode:
                    raise RuntimeError(
                        f"convolution benchmark failed for {tap_count} taps:\n"
                        f"{result.stderr}"
                    )
                measurement = json.loads(result.stdout)
                if measurement["schema"] != "skyapo-bench-v1":
                    raise RuntimeError("unexpected convolution benchmark schema")
                if measurement["filter_count"] != 1:
                    raise RuntimeError(
                        "expected one actual upstream Convolution filter"
                    )
                for metric in (
                    "mean_us", "median_us", "p95_us", "max_us",
                    "block_budget_us", "max_rss_kib", "p95_budget_percent",
                ):
                    if (
                        not isinstance(measurement[metric], (float, int))
                        or not math.isfinite(measurement[metric])
                        or measurement[metric] < 0
                    ):
                        raise RuntimeError(f"invalid convolution metric {metric}")
                if measurement["max_rss_kib"] <= 0:
                    raise RuntimeError("convolution process peak RSS was not reported")
                if measurement["measured_blocks"] != 40:
                    raise RuntimeError(
                        "convolution measured block count was not recorded"
                    )
            print("skyapo-bench measured upstream convolution with 3/257/4097-tap IRs.")
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
            if measurement["schema"] != "skyapo-bench-v1":
                raise RuntimeError("unexpected benchmark schema")
            if measurement["filter_count"] != count:
                raise RuntimeError(
                    f"expected {count} actual filters, got {measurement['filter_count']}"
                )
            for metric in (
                "mean_us", "median_us", "p95_us", "max_us",
                "block_budget_us", "max_rss_kib", "p95_budget_percent",
            ):
                if (
                    not isinstance(measurement[metric], (float, int))
                    or not math.isfinite(measurement[metric])
                    or measurement[metric] < 0
                ):
                    raise RuntimeError(f"invalid benchmark metric {metric}")
            if measurement["max_rss_kib"] <= 0:
                raise RuntimeError("process peak RSS was not reported")
            if measurement["measured_blocks"] != 40:
                raise RuntimeError("measured block count was not recorded")
    print("skyapo-bench measured 1/4/16 actual upstream Preamp filters.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"benchmark test failed: {error}", file=sys.stderr)
        sys.exit(1)
