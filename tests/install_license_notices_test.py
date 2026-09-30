#!/usr/bin/env python3
"""Smoke-test the license/attribution files in a staged CMake install."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        raise RuntimeError("usage: install_license_notices_test.py CMAKE BUILD_DIR")
    cmake, build_dir = sys.argv[1], pathlib.Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="skyapo-license-install-") as temp:
        prefix = pathlib.Path(temp)
        result = subprocess.run(
            [cmake, "--install", str(build_dir), "--prefix", str(prefix)],
            text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(
                f"staged install failed:\n{result.stdout}\n{result.stderr}")

        doc_roots = list((prefix / "share" / "doc").glob("*"))
        if len(doc_roots) != 1:
            raise RuntimeError(f"expected one installed doc root, found {doc_roots}")
        docs = doc_roots[0]
        licenses = docs / "licenses"
        expected = {
            "EqualizerAPO-License.txt": "GNU GENERAL PUBLIC LICENSE",
            "CLAP-License.txt": "Copyright (c) 2021 Alexandre BIQUE",
            "Steinberg-VST3-base-License.txt": "Steinberg Media Technologies GmbH",
            "Steinberg-VST3-pluginterfaces-License.txt": "Steinberg Media Technologies GmbH",
            "Steinberg-VST3-public-sdk-License.txt": "Steinberg Media Technologies GmbH",
        }
        cache = (build_dir / "CMakeCache.txt").read_text(errors="replace")
        if "SKYAPO_ENABLE_MUPARSERX:BOOL=ON" in cache:
            expected["MuParserX-License.txt"] = "Copyright (c) 2012, Ingo Berg"
        for relative, marker in expected.items():
            path = licenses / relative
            text = (path.read_text(errors="replace").replace("\u00a0", " ")
                    if path.is_file() else "")
            if marker not in text:
                raise RuntimeError(f"missing/incorrect installed license: {path}")
        notice = docs / "THIRD_PARTY_NOTICES.md"
        if not notice.is_file() or "FST-based VST2" not in notice.read_text():
            raise RuntimeError("third-party notice inventory was not installed")
        print("Installed GPL, MIT and BSD license notices verified.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"license install test failed: {error}", file=sys.stderr)
        sys.exit(1)
