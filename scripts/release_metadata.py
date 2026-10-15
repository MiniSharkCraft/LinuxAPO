#!/usr/bin/env python3
"""Prepare and validate version-specific LinuxAPO release metadata."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


VERSION_RE = re.compile(r"^0\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")
REQUIRED_SECTIONS = ("Purpose", "Verified test results", "Known issues and limitations")
PLACEHOLDER_RE = re.compile(r"\b(TODO|TBD|PLACEHOLDER|DRAFT ONLY)\b", re.I)
INSTRUCTION_RE = re.compile(r"\[(?:draft|required):|record only commands|explain the verified|list limitations", re.I)


class MetadataError(ValueError):
    pass


def release_title(tag: str, notes: str) -> str:
    titles = [line[2:].strip() for line in notes.splitlines() if line.startswith("# ")]
    if not titles:
        raise MetadataError(f"release notes for {tag} have no level-one title")
    title = titles[0]
    if len(titles) != 1 or not title.startswith(f"LinuxAPO {tag} Beta | "):
        raise MetadataError(
            f"title must be one descriptive heading beginning "
            f"'LinuxAPO {tag} Beta | ': {title!r}"
        )
    topic = title.partition("|")[2].strip()
    if len(topic) < 4 or topic.endswith(("|", ".")):
        raise MetadataError(f"release title needs a meaningful unique topic: {title!r}")
    return title


def validate_release(root: pathlib.Path, tag: str) -> str:
    if not tag.startswith("v") or not VERSION_RE.fullmatch(tag[1:]):
        raise MetadataError(f"automatic prereleases only support v0.x.y tags, got {tag!r}")

    notes_path = root / "docs" / "releases" / f"{tag}.md"
    changelog_path = root / "CHANGELOG.md"
    if not notes_path.is_file() or not notes_path.stat().st_size:
        raise MetadataError(f"missing committed version-specific notes: {notes_path}")
    if not changelog_path.is_file():
        raise MetadataError(f"missing changelog: {changelog_path}")

    notes = notes_path.read_text(encoding="utf-8")
    title = release_title(tag, notes)
    body = notes.split("\n", 1)[1] if "\n" in notes else ""
    for section in REQUIRED_SECTIONS:
        if not re.search(rf"^##\s+{re.escape(section)}\s*$", body, re.M | re.I):
            raise MetadataError(f"{notes_path} must include a '## {section}' section")
    if len(body.strip()) < 120:
        raise MetadataError(f"release notes for {tag} are too short to explain the release")
    if PLACEHOLDER_RE.search(notes) or INSTRUCTION_RE.search(notes):
        raise MetadataError(f"release notes for {tag} contain unfinished placeholder text")

    changelog = changelog_path.read_text(encoding="utf-8")
    version = re.escape(tag[1:])
    if not re.search(rf"^##\s+v?{version}(?:\s|$)", changelog, re.M):
        raise MetadataError(f"CHANGELOG.md has no version entry for {tag}")

    topic = title.partition("|")[2].strip().casefold()
    for previous in (root / "docs" / "releases").glob("v*.md"):
        if previous == notes_path:
            continue
        previous_notes = previous.read_text(encoding="utf-8")
        previous_titles = [
            line[2:].strip() for line in previous_notes.splitlines()
            if line.startswith("# ")
        ]
        if previous_titles and previous_titles[0].partition("|")[2].strip().casefold() == topic:
            raise MetadataError(
                f"release topic is reused from {previous.relative_to(root)}: {topic}"
            )
    return title


def git(root: pathlib.Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return result.stdout


def render_draft(
    version: str, previous_tag: str, topic: str,
    commits: list[tuple[str, str]], changed_files: list[str],
) -> str:
    tag = f"v{version}"
    title = f"LinuxAPO {tag} Beta | {topic.strip()}"
    commit_lines = "\n".join(f"- `{sha}` {subject}" for sha, subject in commits)
    file_lines = "\n".join(f"- `{name}`" for name in changed_files)
    if not commit_lines:
        commit_lines = "- No commits found in the requested range."
    if not file_lines:
        file_lines = "- No changed paths found in the requested range."
    return (
        f"# {title}\n\n"
        "> Draft generated from the Git range and changed paths below. Replace this notice and "
        "review the actual source, milestone scope and final test evidence before committing.\n\n"
        f"## Purpose\n\n"
        f"Explain the verified user-facing purpose of this release ({topic.strip()}).\n\n"
        f"## Changes since {previous_tag}\n\n"
        "The following items are gathered from Git history and changed paths; "
        "review them against the final source and rewrite them in user-facing language.\n\n"
        f"{commit_lines}\n\n"
        "### Changed paths\n\n"
        f"{file_lines}\n\n"
        "## Verified test results\n\n"
        "[REQUIRED: replace with exact commands actually run on the final release commit and results.]\n\n"
        "## Known issues and limitations\n\n"
        "[REQUIRED: document known limitations after checking the current roadmap.]\n"
    )


def prepare_draft(root: pathlib.Path, version: str, topic: str, previous_tag: str | None) -> pathlib.Path:
    if not VERSION_RE.fullmatch(version):
        raise MetadataError(f"draft version must be 0.x.y, got {version!r}")
    if not topic.strip() or "|" in topic or "\n" in topic:
        raise MetadataError("provide a concise, unique title topic without '|' or newlines")
    if previous_tag is None:
        previous_tag = git(root, "describe", "--tags", "--match", "v0.*", "--abbrev=0", "HEAD").strip()
    if not re.fullmatch(r"v0\.[0-9]+\.[0-9]+", previous_tag):
        raise MetadataError(f"previous release must be an existing v0.x.y tag, got {previous_tag!r}")
    previous_version = tuple(int(part) for part in previous_tag[1:].split("."))
    requested_version = tuple(int(part) for part in version.split("."))
    if requested_version <= previous_version:
        raise MetadataError(
            f"draft version {version} must be newer than previous release {previous_tag}"
        )
    if git(root, "tag", "--list", f"v{version}").strip():
        raise MetadataError(f"refusing to prepare metadata for an existing tag: v{version}")
    try:
        git(root, "rev-parse", "--verify", f"refs/tags/{previous_tag}")
    except subprocess.CalledProcessError as exc:
        raise MetadataError(f"previous release tag does not exist: {previous_tag}") from exc
    output = root / "docs" / "releases" / f"v{version}.md"
    if output.exists():
        raise MetadataError(f"refusing to overwrite existing release notes: {output}")
    commit_rows = []
    for line in git(root, "log", "--no-decorate", "--format=%h%x09%s", f"{previous_tag}..HEAD").splitlines():
        sha, _, subject = line.partition("\t")
        commit_rows.append((sha, subject))
    changed_files = sorted(set(filter(None, git(
        root, "diff", "--name-only", f"{previous_tag}...HEAD"
    ).splitlines())))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(render_draft(version, previous_tag, topic, commit_rows, changed_files), encoding="utf-8")
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate committed metadata for a release tag")
    validate.add_argument("tag")
    validate.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    draft = subparsers.add_parser("draft", help="create a release-note draft from Git history")
    draft.add_argument("version", help="0.x.y version without the leading v")
    draft.add_argument("--topic", required=True, help="specific, accurate release-title topic")
    draft.add_argument("--previous-tag", help="prior published v0.x.y tag; inferred when omitted")
    draft.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)
    try:
        if args.command == "validate":
            title = validate_release(args.root.resolve(), args.tag)
            print(title)
        else:
            output = prepare_draft(args.root.resolve(), args.version, args.topic, args.previous_tag)
            print(f"Created draft {output}; review source changes and add exact final-commit test evidence before tagging.")
        return 0
    except (MetadataError, OSError, subprocess.CalledProcessError) as exc:
        detail = exc.stderr.strip() if isinstance(exc, subprocess.CalledProcessError) else str(exc)
        print(f"release-metadata: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
