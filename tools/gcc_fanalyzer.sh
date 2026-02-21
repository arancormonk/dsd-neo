#!/usr/bin/env bash
set -euo pipefail

# Run GCC's static analyzer (-fanalyzer) using compile_commands.json.
# - Supports targeted translation units
# - Excludes build/ and src/third_party/
# - By default, fails on analyzer diagnostics or compiler errors

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/gcc_fanalyzer.sh [--strict] [--all-commands] [--jobs N] [--] [files...]

Options:
  --strict        Treat all compiler warnings as failures (not just analyzer diagnostics).
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

if ! command -v gcc >/dev/null 2>&1; then
  echo "gcc not found. Please install GCC." >&2
  exit 1
fi
if ! command -v g++ >/dev/null 2>&1; then
  echo "g++ not found. Please install G++." >&2
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

LOG_FILE=".gcc-fanalyzer.local.out"

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
    print("No translation units selected for GCC analyzer.")
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
    f"Running GCC -fanalyzer on {len(selected_entries)} compile command(s) "
    f"(strict={'yes' if strict else 'no'}, jobs={jobs})..."
)

analyzer_diag = re.compile(r"\[-Wanalyzer-[^]]+\]")
compiler_error = re.compile(r"(^|\\s)error:", re.IGNORECASE)
compiler_warning = re.compile(r"(^|\\s)warning:", re.IGNORECASE)


def transform_command(rel, entry):
    tokens = tokens_for_entry(entry)
    if not tokens:
        return None, "No compile command tokens found for this entry."

    cmd = list(tokens)
    compiler_index = 0
    if pathlib.Path(cmd[0]).name in {"ccache", "sccache"} and len(cmd) > 1:
        compiler_index = 1

    suffix = pathlib.Path(rel).suffix.lower()
    desired_compiler = "gcc" if suffix == ".c" else "g++"
    cmd[compiler_index] = desired_compiler

    filtered = []
    skip_next = False
    for token in cmd:
        if skip_next:
            skip_next = False
            continue
        if token in {"-o", "-MF", "-MT", "-MQ", "--output"}:
            skip_next = True
            continue
        if token in {"-MMD", "-MD", "-c"}:
            continue
        filtered.append(token)

    filtered.extend(["-fsyntax-only", "-fanalyzer", "-fdiagnostics-color=never"])
    if strict:
        filtered.append("-Werror")

    return filtered, ""


def run_gcc_analyzer(rel, entry):
    cmd, err = transform_command(rel, entry)
    if not cmd:
        return {
            "rel": rel,
            "output": err,
            "fatal": True,
            "analyzer_hits": 0,
        }

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
            "output": f"Failed to execute GCC analyzer command: {exc}",
            "fatal": True,
            "analyzer_hits": 0,
        }

    output = proc.stdout or ""
    analyzer_hits = len(analyzer_diag.findall(output))
    has_error = bool(compiler_error.search(output))
    has_warning = bool(compiler_warning.search(output))

    fatal = has_error or analyzer_hits > 0
    if strict and has_warning:
        fatal = True

    if proc.returncode != 0 and not fatal:
        # Conservative: non-zero without recognized diagnostics still fails.
        fatal = True

    return {
        "rel": rel,
        "output": output.strip(),
        "fatal": fatal,
        "analyzer_hits": analyzer_hits,
    }


results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
    futures = [executor.submit(run_gcc_analyzer, rel, entry) for rel, entry in selected_entries]
    for future in concurrent.futures.as_completed(futures):
        results.append(future.result())

results.sort(key=lambda r: r["rel"])

fatal_count = 0
hit_count = 0
for result in results:
    print(f"\n===== GCC analyzer: {result['rel']} =====")
    if result["output"]:
        print(result["output"])
    if result["fatal"]:
        fatal_count += 1
    hit_count += result["analyzer_hits"]

print(
    f"\nGCC analyzer summary: analyzed={len(results)} analyzer_diagnostics={hit_count} fatal={fatal_count}"
)

if fatal_count > 0:
    raise SystemExit(1)
raise SystemExit(0)
PY
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "GCC analyzer completed. Full output in $LOG_FILE"
else
  echo "GCC analyzer found issues. See $LOG_FILE for details." >&2
fi

exit "$rc"
