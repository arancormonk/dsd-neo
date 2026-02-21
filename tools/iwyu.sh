#!/usr/bin/env bash
set -euo pipefail

# Run include-what-you-use (IWYU) using the project's compilation database.
# - Supports targeted translation units
# - Excludes build/ and src/third_party/
# - Optional strict mode fails on include suggestions

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/iwyu.sh [--strict] [--all-commands] [--jobs N] [--] [files...]

Options:
  --strict        Fail when IWYU suggests include changes.
  --all-commands  Analyze every compile command entry (including duplicates).
  --jobs N        Number of parallel workers (default: detected CPU count).

Arguments:
  files...        Optional list of translation units to analyze (e.g., src/foo.c).
                  When omitted, analyzes all translation units in the compilation
                  database after filtering.
USAGE
}

STRICT=0
ALL_COMMANDS=0
JOBS=""
REQUESTED_FILES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict) STRICT=1; shift ;;
    --all-commands) ALL_COMMANDS=1; shift ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --jobs" >&2
        exit 2
      fi
      JOBS="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    --) shift; REQUESTED_FILES+=("$@"); break ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *) REQUESTED_FILES+=("$1"); shift ;;
  esac
done

if ! command -v include-what-you-use >/dev/null 2>&1; then
  echo "include-what-you-use not found. Please install it (e.g., apt-get install iwyu)." >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found. Please install python3." >&2
  exit 1
fi

# Prefer compile_commands from dev-debug; otherwise use top-level if present.
PDB_DIR="build/dev-debug"
PDB_FILE="$PDB_DIR/compile_commands.json"
if [[ ! -f "$PDB_FILE" ]]; then
  if [[ -f "compile_commands.json" ]]; then
    PDB_DIR="."
    PDB_FILE="compile_commands.json"
  else
    echo "Configuring CMake preset 'dev-debug' to generate compile_commands.json..."
    cmake --preset dev-debug >/dev/null
    PDB_FILE="$PDB_DIR/compile_commands.json"
  fi
fi

if [[ -z "$JOBS" ]]; then
  JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
fi

LOG_FILE=".iwyu.local.out"

set +e
python3 - "$PDB_FILE" "$ROOT_DIR" "$STRICT" "$ALL_COMMANDS" "$JOBS" "${REQUESTED_FILES[@]}" <<'PY' 2>&1 | tee "$LOG_FILE"
import concurrent.futures
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

pdb_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2]).resolve()
strict = bool(int(sys.argv[3]))
all_commands = bool(int(sys.argv[4]))
jobs = max(1, int(sys.argv[5]))
requested = sys.argv[6:]

try:
    data = json.loads(pdb_path.read_text())
except Exception as exc:
    raise SystemExit(f"Failed to read {pdb_path}: {exc}")


def normalize_file(entry):
    file_field = entry.get("file")
    if not file_field:
        return None, None

    file_path = pathlib.Path(file_field)
    if not file_path.is_absolute():
        directory = entry.get("directory") or str(pdb_path.parent)
        file_path = pathlib.Path(directory) / file_path
    try:
        file_path = file_path.resolve()
    except Exception:
        file_path = file_path.absolute()

    try:
        rel = file_path.relative_to(root)
    except ValueError:
        return None, None

    if rel.parts and rel.parts[0] == "build":
        return None, None
    if len(rel.parts) >= 2 and rel.parts[0] == "src" and rel.parts[1] == "third_party":
        return None, None

    return rel.as_posix(), str(file_path)


def tokens_for_entry(entry):
    args = entry.get("arguments")
    if args:
        return list(args)
    cmd = entry.get("command") or ""
    if not cmd:
        return []
    try:
        return shlex.split(cmd)
    except Exception:
        return cmd.split()


def score_entry(entry):
    tokens = tokens_for_entry(entry)
    score = 0
    if any("/tests/" in t or t.startswith("tests/") for t in tokens):
        score -= 1000
    if any("tests/" in t for t in tokens if t.startswith("-I")):
        score -= 250
    if "-fPIC" in tokens:
        score += 50
    if "-fPIE" in tokens:
        score -= 10
    score += sum(1 for t in tokens if t.startswith("-D"))
    score += sum(1 for t in tokens if t.startswith("-I"))
    score += min(len(tokens), 500) // 10
    return score


def is_translation_unit(path):
    return pathlib.Path(path).suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}


entries_by_rel = {}
total_entries = 0
for entry in data:
    rel, _ = normalize_file(entry)
    if not rel:
        continue
    total_entries += 1
    entries_by_rel.setdefault(rel, []).append(entry)

unique_files = sorted(entries_by_rel.keys())
multi_cmd_files = sum(1 for cmds in entries_by_rel.values() if len(cmds) > 1)
extra_cmds = sum(len(cmds) - 1 for cmds in entries_by_rel.values() if len(cmds) > 1)
print(
    f"Compilation database entries: {total_entries} (unique files: {len(unique_files)}, "
    f"files with multiple commands: {multi_cmd_files}, extra commands: {extra_cmds})"
)


def normalize_requested(path_str):
    p = pathlib.Path(path_str)
    if not p.is_absolute():
        p = (root / p).resolve()
    try:
        rel = p.relative_to(root)
    except ValueError:
        return None
    rel_str = rel.as_posix()
    if rel.parts and rel.parts[0] == "build":
        return None
    if len(rel.parts) >= 2 and rel.parts[0] == "src" and rel.parts[1] == "third_party":
        return None
    return rel_str


selected_rel = unique_files
if requested:
    requested_rel = []
    seen = set()
    for req in requested:
        rel = normalize_requested(req)
        if not rel or rel in seen:
            continue
        seen.add(rel)
        requested_rel.append(rel)

    requested_tus = [p for p in requested_rel if is_translation_unit(p)]
    missing = [p for p in requested_tus if p not in entries_by_rel]
    if missing:
        print("Skipping files not present in compilation database:")
        for p in missing:
            print(f"  {p}")
    selected_rel = sorted(p for p in requested_tus if p in entries_by_rel)

if not selected_rel:
    print("No translation units selected for IWYU analysis.")
    raise SystemExit(0)

selected_entries = []
if all_commands:
    for rel in selected_rel:
        for entry in entries_by_rel.get(rel, []):
            selected_entries.append((rel, entry))
else:
    for rel in selected_rel:
        entry = max(entries_by_rel[rel], key=score_entry)
        selected_entries.append((rel, entry))

print(
    f"Running IWYU on {len(selected_entries)} compile command(s) "
    f"(strict={'yes' if strict else 'no'}, jobs={jobs})..."
)

suggest_add_remove = re.compile(r"should (add|remove) these lines:", re.IGNORECASE)
hard_error = re.compile(r"(^|\\s)(fatal error:|error:)", re.IGNORECASE)


def run_iwyu(rel, entry):
    tokens = tokens_for_entry(entry)
    if not tokens:
        return {
            "rel": rel,
            "output": "No compile command tokens found for this entry.",
            "fatal": True,
            "suggested": False,
        }

    cmd = list(tokens)
    compiler_index = 0
    if pathlib.Path(cmd[0]).name in {"ccache", "sccache"} and len(cmd) > 1:
        compiler_index = 1
    cmd[compiler_index] = "include-what-you-use"
    cmd.append("-fno-color-diagnostics")
    if strict:
        cmd.extend(["-Xiwyu", "--error=1"])

    try:
        proc = subprocess.run(
            cmd,
            cwd=entry.get("directory") or str(root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except Exception as exc:
        return {
            "rel": rel,
            "output": f"Failed to execute IWYU command: {exc}",
            "fatal": True,
            "suggested": False,
        }

    output = proc.stdout or ""
    suggested = bool(suggest_add_remove.search(output))
    has_error_text = bool(hard_error.search(output))

    # IWYU may return non-zero when suggestions exist. Treat that as non-fatal in non-strict mode.
    fatal = has_error_text or (proc.returncode != 0 and not suggested)
    if strict and suggested:
        fatal = True

    return {
        "rel": rel,
        "output": output.strip(),
        "fatal": fatal,
        "suggested": suggested,
    }


results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
    futures = [executor.submit(run_iwyu, rel, entry) for rel, entry in selected_entries]
    for future in concurrent.futures.as_completed(futures):
        results.append(future.result())

results.sort(key=lambda r: r["rel"])

fatal_count = 0
suggest_count = 0
for result in results:
    print(f"\n===== IWYU: {result['rel']} =====")
    if result["output"]:
        print(result["output"])
    if result["fatal"]:
        fatal_count += 1
    if result["suggested"]:
        suggest_count += 1

print(
    f"\nIWYU summary: analyzed={len(results)} suggested={suggest_count} fatal={fatal_count}"
)

if fatal_count > 0:
    raise SystemExit(1)
raise SystemExit(0)
PY
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "IWYU completed. Full output in $LOG_FILE"
else
  echo "IWYU found issues. See $LOG_FILE for details." >&2
fi

exit "$rc"
