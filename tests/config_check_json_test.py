#!/usr/bin/env python3
"""Exercise config check's JSON success and parser diagnostics via the CLI."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def run_check(executable: str, config: pathlib.Path):
    return subprocess.run(
        [executable, "config", "check", "--json", str(config)],
        check=False,
        text=True,
        capture_output=True,
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: config_check_json_test.py /path/to/skyapo")
    executable = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="skyapo-config-json-") as temporary:
        root = pathlib.Path(temporary)

        good = root / 'good "config" \\ file.txt'
        good.write_text("Preamp: -6 dB\nFilter: ON PK Fc 1000 Hz Gain 3 dB Q 1\n")
        success = run_check(executable, good)
        assert success.returncode == 0, success.stderr
        result = json.loads(success.stdout)
        assert result["valid"] is True
        assert result["file"] == str(good)
        assert result["filter_count"] == 2
        assert result["diagnostics"] == []
        text_success = subprocess.run(
            [executable, "config", "check", str(good)],
            check=True,
            text=True,
            capture_output=True,
        )
        assert text_success.stdout == "Valid config: 2 filters\n"

        bad = root / 'bad "config" \\ file.txt'
        bad.write_text('Preamp: 0 dB\nUnknownCommand: "bad" \\\n')
        failure = run_check(executable, bad)
        assert failure.returncode != 0
        result = json.loads(failure.stdout)
        assert result["valid"] is False
        assert result["filter_count"] is None
        diagnostic = result["diagnostics"][0]
        assert diagnostic["file"] == str(bad.resolve())
        assert diagnostic["line"] == 2
        assert diagnostic["directive"] == 'UnknownCommand: "bad" \\'
        assert diagnostic["command"] == "UnknownCommand"
        assert "unsupported command" in diagnostic["reason"]
        text_failure = subprocess.run(
            [executable, "config", "check", str(bad)],
            check=False,
            text=True,
            capture_output=True,
        )
        assert text_failure.returncode != 0
        assert text_failure.stdout == ""
        assert "unsupported command 'UnknownCommand'" in text_failure.stderr

        included = root / "included.txt"
        included.write_text("# included config\nFilter: ON PK Fc 30000 Hz Gain 6 dB Q 1\n")
        including = root / "root.txt"
        including.write_text("Preamp: 0 dB\nInclude: included.txt\n")
        nested = run_check(executable, including)
        assert nested.returncode != 0
        result = json.loads(nested.stdout)
        diagnostic = result["diagnostics"][0]
        assert diagnostic["file"] == str(included.resolve())
        assert diagnostic["line"] == 2
        assert diagnostic["command"] == "Filter"
        assert diagnostic["directive"].startswith("Filter:")
        assert "Nyquist" in diagnostic["reason"]

        missing = root / 'missing "config" \\ file.txt'
        absent = run_check(executable, missing)
        assert absent.returncode != 0
        result = json.loads(absent.stdout)
        diagnostic = result["diagnostics"][0]
        assert diagnostic["file"] == str(missing.resolve())
        assert diagnostic["line"] is None
        assert diagnostic["directive"] is None
        assert diagnostic["command"] is None
        assert diagnostic["reason"] == "cannot open config file"

        if os.name == "posix":
            raw_path = os.fsencode(root) + b"/missing-\xff.txt"
            raw_text_path = os.fsdecode(raw_path)
            invalid_utf8 = subprocess.run(
                [executable, "config", "check", "--json", raw_text_path],
                check=False,
                text=True,
                capture_output=True,
            )
            assert invalid_utf8.returncode != 0
            result = json.loads(invalid_utf8.stdout)
            assert "\ufffd" in result["diagnostics"][0]["file"]

    print("config check JSON success, root/include errors, and escaping passed")


if __name__ == "__main__":
    main()
