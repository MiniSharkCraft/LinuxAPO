#!/usr/bin/env python3
"""Check that the Qt UI embeds only resources used by its selected sources."""

import pathlib
import re
import sys
import xml.etree.ElementTree as ET


def resources(qrc: pathlib.Path) -> dict[str, pathlib.Path]:
    result = {}
    for node in ET.parse(qrc).iter("file"):
        alias = node.attrib.get("alias", node.text.strip())
        result[alias] = (qrc.parent / node.text.strip()).resolve()
    return result


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: ui_resource_audit_test.py SOURCE_ROOT FFTW3F_FOUND")
    root = pathlib.Path(sys.argv[1]).resolve()
    graph_enabled = sys.argv[2].lower() in {"1", "on", "true", "yes"}
    base_qrc = root / "src/ui/resources/skyapo_editor.qrc"
    graph_qrc = root / "src/ui/resources/skyapo_graphiceq.qrc"
    embedded = resources(base_qrc)
    if graph_enabled:
        embedded.update(resources(graph_qrc))

    sources = [
        root / "upstream/equalizerapo/Editor/FilterTableRow.cpp",
        root / "upstream/equalizerapo/Editor/FilterTableRow.ui",
    ]
    if graph_enabled:
        sources += [
            root / "upstream/equalizerapo/Editor/widgets/ResizeCorner.cpp",
            root / "upstream/equalizerapo/Editor/guis/GraphicEQFilterGUI.cpp",
        ]
    referenced = set()
    for source in sources:
        contents = source.read_text(encoding="utf-8")
        referenced.update(re.findall(r":/(icons/[A-Za-z0-9_./-]+)", contents))

    if set(embedded) != referenced:
        raise RuntimeError(
            f"resource/source mismatch: missing={sorted(referenced - set(embedded))}, "
            f"unused={sorted(set(embedded) - referenced)}"
        )
    for alias, path in embedded.items():
        if not path.is_file():
            raise RuntimeError(f"resource {alias} points to missing file: {path}")
    if any(path.suffix.lower() in {".qm", ".ts", ".flac"} for path in embedded.values()):
        raise RuntimeError("unused upstream translations/audio were embedded in the UI")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if "${SKYAPO_EDITOR}/Editor.qrc" in cmake:
        raise RuntimeError("the all-upstream Editor.qrc must not be compiled")
    if any(root / "upstream/equalizerapo" in path.parents for path in embedded.values()):
        raise RuntimeError("UI icon resources must not depend on upstream assets")
    if any(path.suffix.lower() != ".svg" for path in embedded.values()):
        raise RuntimeError("UI icon resources must be project-authored SVG assets")
    print(f"Verified {len(embedded)} project-authored SVG UI icon resources.")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"UI resource audit failed: {error}", file=sys.stderr)
        sys.exit(1)
