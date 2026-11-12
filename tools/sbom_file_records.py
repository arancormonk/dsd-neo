#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bind an SPDX SBOM's file records to the files the release asset ships.

Each record is renamed to its path inside the asset and given the SHA1 and
SHA256 of the shipped bytes. A record that cannot be tied to exactly one
shipped file, or that carries a digest the shipped bytes contradict, fails the
run and leaves the document as it was. A rewritten document is written in the
form the other --spdx tools write (two-space indented JSON and a final newline).
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import zipfile

ALGORITHMS = ("SHA1", "SHA256")
CHUNK = 1 << 20
DOS_VOLUME_LABEL = 0x08
DOS_DIRECTORY = 0x10


def is_regular_file(info, path):
    """Return whether a zip entry is a shipped file; every other entry kind is not.

    It must have a name without a trailing slash, DOS attributes marking neither a directory nor a volume label,
    and Unix type bits, when present, marking a regular file.
    """
    kind = stat.S_IFMT(info.external_attr >> 16)
    return (bool(path) and not path.endswith("/")
            and not info.external_attr & (DOS_DIRECTORY | DOS_VOLUME_LABEL)
            and kind in (0, stat.S_IFREG))


def shipped_members(asset):
    """Return the regular-file members of a zip asset by normalised path, or {} for any other file."""
    try:
        archive = zipfile.ZipFile(asset)
    except zipfile.BadZipFile:
        return {}
    names = set()
    members = {}
    with archive:
        for info in archive.infolist():
            path = info.filename.replace("\\", "/")
            if path in names:
                raise ValueError(f"{asset.name} holds more than one member named {path}")
            names.add(path)
            if is_regular_file(info, path):
                members[path] = info
    return members


def hash_stream(stream):
    hashes = [hashlib.new(algorithm.lower()) for algorithm in ALGORITHMS]
    for chunk in iter(lambda: stream.read(CHUNK), b""):
        for digest in hashes:
            digest.update(chunk)
    return [{"algorithm": algorithm, "checksumValue": digest.hexdigest()}
            for algorithm, digest in zip(ALGORITHMS, hashes)]


def shipped_digests(asset, members, path):
    if path in members:
        with zipfile.ZipFile(asset) as archive, archive.open(members[path]) as stream:
            return hash_stream(stream)
    with open(asset, "rb") as stream:
        return hash_stream(stream)


def bind(name, members, asset_name):
    """Return (shipped path, unpack root) for a record name; root is None for the asset itself."""
    matches = [path for path in members if name == path or name.endswith("/" + path)]
    if matches:
        path = max(matches, key=len)
        return path, name[:len(name) - len(path)]
    if name.rsplit("/", 1)[-1] == asset_name:
        return asset_name, None
    return None, None


def checked_checksums(label, record, path, computed):
    checksums = record.get("checksums", [])
    if not isinstance(checksums, list):
        raise ValueError(f"{label} has malformed checksums")
    expected = {entry["algorithm"]: entry["checksumValue"] for entry in computed}
    for entry in checksums:
        if not isinstance(entry, dict):
            raise ValueError(f"{label} has malformed checksums")
        algorithm, value = entry.get("algorithm"), entry.get("checksumValue")
        if algorithm not in expected:
            raise ValueError(f"{label} has unexpected checksum algorithm {algorithm!r}")
        if isinstance(value, str) and value and set(value) == {"0"}:
            continue
        if value != expected[algorithm]:
            raise ValueError(f"{label} {algorithm} {value!r} does not match shipped {path} ({expected[algorithm]})")
    return computed


def bind_files(document, asset):
    """Return the document with every file record bound to the asset; raise ValueError if one cannot be."""
    files = document.get("files")
    if files is None:
        return document
    if not isinstance(files, list):
        raise ValueError("SBOM files is not a list")
    if not files:
        return document
    members = shipped_members(asset)
    root = None
    owners = {}
    bound = []
    for index, record in enumerate(files):
        if not isinstance(record, dict):
            raise ValueError(f"SBOM files[{index}] is not a file record")
        name = record.get("fileName")
        label = f"SBOM file record files[{index}] {record.get('SPDXID', '<no SPDXID>')} ({name!r})"
        path, record_root = bind(name.replace("\\", "/") if isinstance(name, str) else "", members, asset.name)
        if path is None:
            raise ValueError(f"{label} does not name a shipped file of {asset.name}")
        if record_root is not None:
            if root is not None and record_root != root:
                raise ValueError(f"{label} implies unpack root {record_root!r}, other records imply {root!r}")
            root = record_root
        if path in owners:
            raise ValueError(f"{label} binds to {path}, which {owners[path]} already describes")
        owners[path] = label
        bound.append((record, path))
    result = json.loads(json.dumps(document))
    for (record, path), target in zip(bound, result["files"]):
        target["checksums"] = checked_checksums(owners[path], record, path, shipped_digests(asset, members, path))
        target["fileName"] = path
    return result


def replace_document(path, document):
    """Write the document beside the original, then swap it in, so a failed write leaves the original bytes."""
    path = path.resolve()  # through a symlink, replace the real file and keep the link
    mode = stat.S_IMODE(path.stat().st_mode)
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with open(handle, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(document, indent=2) + "\n")
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spdx", type=Path, required=True, help="SPDX JSON SBOM to update in place before attestation")
    parser.add_argument("--asset", type=Path, required=True, help="the release asset the SBOM describes")
    args = parser.parse_args()
    document = json.loads(args.spdx.read_text(encoding="utf-8"))
    result = bind_files(document, args.asset)
    if result != document:
        replace_document(args.spdx, result)
    print(f"SBOM file records bound to {args.asset.name}: {len(result.get('files') or [])}")


if __name__ == "__main__":
    main()
