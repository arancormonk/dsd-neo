#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drop CodeQL results whose primary location matches the config's paths-ignore.

CodeQL applies paths-ignore only to files it indexes without a build. A traced
C/C++ build (build-mode: manual) analyzes every file the compiler sees, so
vendored sources that tests compile, such as android/third_party/librtlsdr,
reach code scanning as project alerts. The workflow saves the SARIF instead of
uploading it, runs this filter with the same config file, and uploads the
result, so one paths-ignore list governs every language.

The config reader is deliberately strict: it understands plain top-level keys
and a block-sequence paths-ignore list of quoted strings, and fails on anything
else (quoted or merged keys, flow lists, unquoted items, a paths include list,
unsupported glob syntax) rather than filtering less than the config says. No
third-party Python packages are needed in CI.
"""

import argparse
import json
import os
import pathlib
import posixpath
import re
import sys
import urllib.parse

SRCROOT = "%SRCROOT%"
TOP_LEVEL_KEY = re.compile(r"^([A-Za-z0-9_-]+)\s*:(.*)$")
SEQUENCE_ITEM = re.compile(r"^\s*-\s+(.*?)\s*$")
QUOTED_SCALAR = re.compile(r"^(\"[^\"\\]*\"|'[^']*')$")
WINDOWS_DRIVE_PATH = re.compile(r"^/[A-Za-z]:/")


class FilterError(Exception):
    pass


def scalar(text, where):
    # Unquoted YAML items can be booleans, nulls or mappings; quotes keep every
    # item a plain string with no escapes to interpret.
    if not QUOTED_SCALAR.match(text):
        raise FilterError(f"{where}: paths-ignore items must be quoted strings without escapes, not {text}")
    return text[1:-1]


def read_paths_ignore(config_path):
    """Return the top-level paths-ignore list from a CodeQL config file."""
    lines = pathlib.Path(config_path).read_text(encoding="utf-8").splitlines()
    patterns = []
    seen = False
    in_list = False
    for number, line in enumerate(lines, 1):
        where = f"{config_path}:{number}"
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        item = SEQUENCE_ITEM.match(line)
        if in_list and item:
            patterns.append(scalar(item.group(1), where))
            continue
        if line[0] not in " \t":
            in_list = False
            key = TOP_LEVEL_KEY.match(line)
            if not key:
                raise FilterError(f"{where}: expected a plain top-level 'key:' line")
            name, rest = key.group(1), key.group(2).strip()
            if name == "paths":
                raise FilterError(f"{where}: a paths include list is not supported by this filter")
            if name == "paths-ignore":
                if seen:
                    raise FilterError(f"{where}: duplicate paths-ignore key")
                if rest and not rest.startswith("#"):
                    raise FilterError(f"{where}: paths-ignore must be a block sequence")
                seen = True
                in_list = True
            continue
        if in_list:
            raise FilterError(f"{where}: expected a '- <glob>' paths-ignore item")
    if not patterns:
        raise FilterError(f"{config_path}: no paths-ignore patterns")
    return [compile_pattern(pattern) for pattern in patterns]


def compile_pattern(pattern):
    """Split a paths-ignore glob into segments, checking CodeQL's glob rules."""
    if pattern.startswith("/") or "\\" in pattern:
        raise FilterError(f"paths-ignore {pattern!r}: use a relative path with '/' separators")
    segments = pattern.rstrip("/").split("/")
    for segment in segments:
        if segment in ("", ".", ".."):
            raise FilterError(f"paths-ignore {pattern!r}: empty, '.' or '..' segment")
        if "**" in segment and segment != "**":
            raise FilterError(f"paths-ignore {pattern!r}: '**' must be a whole path segment")
        if re.search(r"[?\[\]{}!]", segment):
            raise FilterError(f"paths-ignore {pattern!r}: only '*' and '**' wildcards are supported")
    return pattern, segments


def segment_matches(glob, name):
    regex = ".*".join(re.escape(part) for part in glob.split("*"))
    return re.fullmatch(regex, name) is not None


def segments_match(globs, names):
    """Match a pattern against a path or any ancestor directory of it."""
    if not globs:
        return True
    if globs[0] == "**":
        return any(segments_match(globs[1:], names[i:]) for i in range(len(names) + 1))
    return bool(names) and segment_matches(globs[0], names[0]) and segments_match(globs[1:], names[1:])


def ignored_by(patterns, path):
    names = path.split("/")
    for pattern, globs in patterns:
        if segments_match(globs, names):
            return pattern
    return None


def file_uri_path(parsed):
    """Return the local path of a parsed file: URI, or None for a remote host."""
    if parsed.netloc not in ("", "localhost"):
        return None
    path = urllib.parse.unquote(parsed.path)
    if WINDOWS_DRIVE_PATH.match(path):
        path = path[1:]
    return pathlib.PurePosixPath(path)


def relative_path(location, run, source_root):
    """Return the repository-relative path of a SARIF location, or None."""
    artifact = (location.get("physicalLocation") or {}).get("artifactLocation") or {}
    if "uri" not in artifact and isinstance(artifact.get("index"), int):
        artifacts = run.get("artifacts") or []
        if 0 <= artifact["index"] < len(artifacts):
            artifact = artifacts[artifact["index"]].get("location") or {}
    uri = artifact.get("uri")
    if not isinstance(uri, str) or not uri:
        return None
    base_id = artifact.get("uriBaseId")
    parsed = urllib.parse.urlsplit(uri)
    if parsed.scheme == "file":
        absolute = file_uri_path(parsed)
        if absolute is None:
            return None
    elif parsed.scheme or uri.startswith("/"):
        return None
    elif base_id in (None, SRCROOT):
        absolute = pathlib.PurePosixPath(source_root, urllib.parse.unquote(parsed.path))
    else:
        base = ((run.get("originalUriBaseIds") or {}).get(base_id) or {}).get("uri", "")
        base_parsed = urllib.parse.urlsplit(base)
        base_path = file_uri_path(base_parsed) if base_parsed.scheme == "file" else None
        if base_path is None:
            return None
        absolute = base_path / urllib.parse.unquote(parsed.path)
    normalized = posixpath.normpath(str(absolute))
    root = posixpath.normpath(source_root)
    if normalized == root or not normalized.startswith(root.rstrip("/") + "/"):
        return None
    return pathlib.PurePosixPath(normalized).relative_to(root).as_posix()


def filter_sarif(sarif, patterns, source_root):
    """Remove ignored results in place; return [(rule, path, line, pattern)]."""
    runs = sarif.get("runs")
    if not isinstance(runs, list):
        raise FilterError("SARIF has no runs array")
    dropped = []
    for run in runs:
        results = run.get("results")
        if not isinstance(results, list):
            continue
        kept = []
        for result in results:
            locations = result.get("locations") or []
            path = relative_path(locations[0], run, source_root) if locations else None
            pattern = ignored_by(patterns, path) if path else None
            if pattern is None:
                kept.append(result)
                continue
            region = (locations[0].get("physicalLocation") or {}).get("region") or {}
            dropped.append((result.get("ruleId", "?"), path, region.get("startLine", "?"), pattern))
        run["results"] = kept
    return dropped


def sarif_files(target):
    target = pathlib.Path(target)
    if target.is_dir():
        files = sorted(target.glob("*.sarif"))
        if not files:
            raise FilterError(f"{target}: no .sarif files")
        return files
    if not target.is_file():
        raise FilterError(f"{target}: no such SARIF file or directory")
    return [target]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Drop CodeQL results whose primary location matches the config's paths-ignore."
    )
    parser.add_argument("--config", required=True, help="CodeQL config file with paths-ignore")
    parser.add_argument(
        "--source-root",
        default=os.environ.get("GITHUB_WORKSPACE") or os.getcwd(),
        help="checkout the SARIF paths are relative to (default: $GITHUB_WORKSPACE or the cwd)",
    )
    parser.add_argument("sarif", help="SARIF file, or a directory of .sarif files, rewritten in place")
    args = parser.parse_args(argv)
    try:
        patterns = read_paths_ignore(args.config)
        source_root = pathlib.Path(args.source_root).absolute().as_posix()
        for path in sarif_files(args.sarif):
            try:
                sarif = json.loads(path.read_text(encoding="utf-8"))
            except ValueError as exc:
                raise FilterError(f"{path}: invalid JSON: {exc}") from exc
            dropped = filter_sarif(sarif, patterns, source_root)
            for rule, rel, line, pattern in dropped:
                print(f"{path.name}: dropped {rule} at {rel}:{line} (paths-ignore {pattern})")
            if dropped:
                path.write_text(json.dumps(sarif, separators=(",", ":")), encoding="utf-8")
            print(f"{path.name}: dropped {len(dropped)} result(s) under paths-ignore")
    except (FilterError, OSError) as exc:
        print(f"sarif_drop_ignored_paths: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
