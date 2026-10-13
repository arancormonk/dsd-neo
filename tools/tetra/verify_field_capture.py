#!/usr/bin/env python3
"""Validate a TETRA CC -> VC -> CC field-capture evidence bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

DEFAULT_LOG_SEQUENCE = (
    r"\[TETRA BSCH\]",
    r"\[TETRA CHANNEL-ALLOCATION\]",
    r"\[TETRA TCH/FS\]",
    r"\[TETRA BSCH\]",
)
PROVENANCE_FIELDS = (
    "Capture UTC", "Location", "Receiver", "Antenna", "Operator/source",
    "Control channel Hz", "Traffic channel Hz",
)


class ValidationError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def _integer(value: Any, name: str, minimum: int = 0) -> int:
    _require(isinstance(value, int) and not isinstance(value, bool), f"{name} must be an integer")
    _require(value >= minimum, f"{name} must be >= {minimum}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _data_path(metadata_path: Path, metadata: dict[str, Any]) -> Path:
    value = metadata.get("data_file")
    _require(isinstance(value, str) and value, "data_file must be a non-empty string")
    path = Path(value)
    if not path.is_absolute():
        path = metadata_path.parent / path
    return path.resolve()


def _ordered_log_matches(text: str, patterns: Iterable[str]) -> list[dict[str, Any]]:
    position = 0
    matches: list[dict[str, Any]] = []
    for pattern in patterns:
        try:
            match = re.search(pattern, text[position:], flags=re.MULTILINE)
        except re.error as exc:
            raise ValidationError(f"invalid log regular expression {pattern!r}: {exc}") from exc
        _require(match is not None, f"decode log is missing ordered milestone {pattern!r}")
        absolute = position + match.start()
        matches.append({"pattern": pattern, "offset": absolute})
        position += match.end()
    return matches


def _validate_provenance(path: Path, cc_hz: int, vc_hz: int) -> None:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ValidationError(f"cannot read provenance file: {exc}") from exc
    for field in PROVENANCE_FIELDS:
        match = re.search(rf"(?im)^\s*(?:[-*]\s*)?{re.escape(field)}\s*:\s*(.+?)\s*$", text)
        _require(match is not None and match.group(1).strip(),
                 f"provenance is missing non-empty '{field}:'")
    for field, expected in (("Control channel Hz", cc_hz), ("Traffic channel Hz", vc_hz)):
        match = re.search(rf"(?im)^\s*(?:[-*]\s*)?{re.escape(field)}\s*:\s*([0-9]+)\s*$", text)
        _require(match is not None and int(match.group(1)) == expected,
                 f"provenance {field} does not match {expected}")


def validate_capture(
    metadata_path: Path,
    *,
    cc_hz: int | None = None,
    vc_hz: int | None = None,
    decode_log: Path | None = None,
    provenance: Path | None = None,
    acceptance: bool = False,
    log_patterns: Iterable[str] = DEFAULT_LOG_SEQUENCE,
) -> dict[str, Any]:
    metadata_path = metadata_path.resolve()
    _require(metadata_path.is_file(), f"metadata file does not exist: {metadata_path}")
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read metadata: {exc}") from exc
    _require(isinstance(metadata, dict), "metadata root must be an object")
    _require(metadata.get("format") == "dsd-neo-iq", "format must be 'dsd-neo-iq'")
    _require(metadata.get("version") == 2, "retune evidence requires metadata version 2")

    sample_format = metadata.get("sample_format")
    alignment = {"cu8": 2, "cs16": 4, "cf32": 8}.get(sample_format)
    _require(alignment is not None, "sample_format must be cu8, cs16, or cf32")
    sample_rate = _integer(metadata.get("sample_rate_hz"), "sample_rate_hz", 1)
    data_bytes = _integer(metadata.get("data_bytes"), "data_bytes", 1)
    _require(data_bytes % alignment == 0, f"data_bytes is not {alignment}-byte sample aligned")

    data_path = _data_path(metadata_path, metadata)
    _require(data_path.is_file(), f"capture data file does not exist: {data_path}")
    actual_bytes = data_path.stat().st_size
    _require(actual_bytes == data_bytes,
             f"data size mismatch: metadata={data_bytes}, actual={actual_bytes}")

    for field in ("capture_drops", "capture_drop_blocks", "input_ring_drops"):
        _require(_integer(metadata.get(field), field) == 0, f"{field} must be zero")
    _require(metadata.get("contains_retunes") is True, "contains_retunes must be true")
    retune_count = _integer(metadata.get("capture_retune_count"), "capture_retune_count", 2)
    for field in ("capture_started_utc", "source_backend", "source_args"):
        _require(isinstance(metadata.get(field), str) and metadata[field],
                 f"{field} must be a non-empty string")

    events = metadata.get("events")
    _require(isinstance(events, list) and events, "events must be a non-empty array")
    previous_offset = -1
    retunes: list[dict[str, int]] = []
    pending_retune_center: int | None = None
    completed_resets = 0
    for index, event in enumerate(events):
        _require(isinstance(event, dict), f"events[{index}] must be an object")
        kind = event.get("kind")
        _require(kind in {"RETUNE", "MUTE", "RESET"}, f"events[{index}].kind is invalid")
        offset = _integer(event.get("byte_offset"), f"events[{index}].byte_offset")
        _require(offset >= previous_offset, "event byte offsets must be nondecreasing")
        _require(offset <= data_bytes, f"events[{index}] is past the end of capture data")
        _require(offset % alignment == 0, f"events[{index}] offset is not sample aligned")
        previous_offset = offset
        if kind == "MUTE":
            duration = _integer(event.get("duration_bytes"), f"events[{index}].duration_bytes", 1)
            _require(duration % alignment == 0,
                     f"events[{index}] mute duration is not sample aligned")
        else:
            rate = _integer(event.get("sample_rate_hz"), f"events[{index}].sample_rate_hz", 1)
            _require(rate == sample_rate, f"events[{index}] changes sample rate")
            center = _integer(event.get("center_frequency_hz"),
                              f"events[{index}].center_frequency_hz", 1)
            _integer(event.get("capture_center_frequency_hz"),
                     f"events[{index}].capture_center_frequency_hz", 1)
            if kind == "RETUNE":
                _require(pending_retune_center is None,
                         f"events[{index}] starts a RETUNE before the preceding RESET")
                pending_retune_center = center
                retunes.append({"index": index, "byte_offset": offset, "center_frequency_hz": center})
            elif pending_retune_center is not None:
                _require(center == pending_retune_center,
                         f"events[{index}] RESET center does not match its RETUNE")
                pending_retune_center = None
                completed_resets += 1

    _require(pending_retune_center is None, "final RETUNE has no following RESET")
    _require(len(retunes) == retune_count,
             f"capture_retune_count={retune_count}, but timeline contains {len(retunes)} RETUNE events")
    _require(completed_resets == retune_count,
             f"timeline completes {completed_resets} RETUNE/RESET pairs, expected {retune_count}")
    initial_cc = _integer(metadata.get("center_frequency_hz"), "center_frequency_hz", 1)
    if cc_hz is not None:
        _require(initial_cc == cc_hz,
                 f"initial center {initial_cc} Hz does not match requested CC {cc_hz} Hz")
    else:
        cc_hz = initial_cc

    vc_candidates = [event for event in retunes if event["center_frequency_hz"] != cc_hz]
    _require(vc_candidates, f"timeline never leaves control channel {cc_hz} Hz")
    if vc_hz is None:
        vc_hz = vc_candidates[0]["center_frequency_hz"]
    to_vc = next((event for event in retunes if event["center_frequency_hz"] == vc_hz), None)
    _require(to_vc is not None, f"timeline never retunes to voice channel {vc_hz} Hz")
    back_to_cc = next((event for event in retunes
                       if event["index"] > to_vc["index"]
                       and event["center_frequency_hz"] == cc_hz), None)
    _require(back_to_cc is not None, f"timeline does not return to control channel {cc_hz} Hz")
    _require(back_to_cc["byte_offset"] > to_vc["byte_offset"],
             "control-channel return must occur after captured voice-channel samples")

    if acceptance:
        _require(decode_log is not None, "acceptance mode requires --decode-log")
        _require(provenance is not None, "acceptance mode requires --provenance")
    hashes = {"metadata_sha256": _sha256(metadata_path), "data_sha256": _sha256(data_path)}
    log_matches: list[dict[str, Any]] = []
    if decode_log is not None:
        decode_log = decode_log.resolve()
        _require(decode_log.is_file(), f"decode log does not exist: {decode_log}")
        try:
            log_text = decode_log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise ValidationError(f"cannot read decode log: {exc}") from exc
        log_matches = _ordered_log_matches(log_text, log_patterns)
        hashes["decode_log_sha256"] = _sha256(decode_log)
    if provenance is not None:
        provenance = provenance.resolve()
        _require(provenance.is_file() and provenance.stat().st_size > 0,
                 f"provenance file is missing or empty: {provenance}")
        _validate_provenance(provenance, cc_hz, vc_hz)
        hashes["provenance_sha256"] = _sha256(provenance)

    return {
        "status": "accepted" if acceptance else "structurally-valid",
        "metadata": str(metadata_path), "data": str(data_path),
        "sample_format": sample_format, "sample_rate_hz": sample_rate,
        "data_bytes": data_bytes, "duration_seconds": data_bytes / alignment / sample_rate,
        "control_channel_hz": cc_hz, "voice_channel_hz": vc_hz,
        "to_voice_byte_offset": to_vc["byte_offset"],
        "return_to_control_byte_offset": back_to_cc["byte_offset"],
        "retune_count": retune_count, "decode_log_milestones": log_matches, "hashes": hashes,
        "decode_log": str(decode_log) if decode_log is not None else None,
        "provenance": str(provenance) if provenance is not None else None,
    }


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path, help="dsd-neo IQ metadata JSON")
    parser.add_argument("--cc-hz", type=int, help="expected control-channel center frequency")
    parser.add_argument("--vc-hz", type=int, help="expected traffic-channel center frequency")
    parser.add_argument("--decode-log", type=Path, help="stderr/stdout log from realtime replay")
    parser.add_argument("--provenance", type=Path, help="capture source and conditions document")
    parser.add_argument("--acceptance", action="store_true",
                        help="require provenance and ordered CC/grant/TCH/CC decode evidence")
    parser.add_argument("--require-log", action="append", default=None, metavar="REGEX",
                        help="ordered decode-log regex; repeat to override the default sequence")
    parser.add_argument("--report", type=Path, help="write the validation report as JSON")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if args.acceptance and args.require_log is not None:
        print("FAIL: --acceptance uses fixed TETRA milestones; --require-log is not allowed", file=sys.stderr)
        return 1
    patterns = args.require_log if args.require_log is not None else DEFAULT_LOG_SEQUENCE
    try:
        report = validate_capture(args.metadata, cc_hz=args.cc_hz, vc_hz=args.vc_hz,
                                  decode_log=args.decode_log, provenance=args.provenance,
                                  acceptance=args.acceptance, log_patterns=patterns)
    except ValidationError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
