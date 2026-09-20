#!/usr/bin/env bash
set -euo pipefail

# Run clang-tidy locally in a way that mirrors CI:
# - Ensures a compile_commands.json database exists (dev-debug preset)
# - Analyzes translation units in the compilation database using the repo's .clang-tidy
# - Fails if any diagnostics are emitted as errors (WarningsAsErrors), or if clang-tidy can't process a file

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/clang_tidy.sh [--all-commands] [--jobs N] [--] [files...]

Options:
  --all-commands  Keep every compile command for each file rather than the single
                  best-scoring one. Note: if a source file appears multiple times
                  in the compilation database (e.g., built for multiple targets),
                  clang-tidy may process it multiple times and its progress
                  counter can exceed the unique file count.
  --jobs N        Number of translation units analyzed at once. Overrides
                  DSD_CLANG_TIDY_JOBS; without either, the CPU count, capped by
                  the available memory at about 1 GB per worker.

Arguments:
  files...        Optional list of translation units to analyze (e.g., src/foo.c).
                  When omitted, analyzes all translation units in the compilation
                  database. Non-translation-unit paths (e.g., headers) are ignored.

Environment:
  DSD_CLANG_TIDY_JOBS  Default parallelism when --jobs is not given (each
                       worker is one clang-tidy process, roughly 200 MB).
USAGE
}

ALL_COMMANDS=0
JOBS="${DSD_CLANG_TIDY_JOBS:-}"
REQUESTED_FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --all-commands)
      ALL_COMMANDS=1
      shift
      ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --jobs" >&2
        exit 2
      fi
      JOBS="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      REQUESTED_FILES+=("$@")
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      REQUESTED_FILES+=("$1")
      shift
      ;;
  esac
done

if ! command -v clang-tidy > /dev/null 2>&1; then
  echo "clang-tidy not found. Please install it (e.g., apt-get install clang-tidy)." >&2
  exit 1
fi
if ! command -v rg > /dev/null 2>&1; then
  echo "ripgrep (rg) not found. Please install it (e.g., apt-get install ripgrep)." >&2
  exit 1
fi

# Sized by memory as well as cores: a worker here holds a few hundred MB on an
# ordinary translation unit and close to a gigabyte on the Qt ones, and a worker
# the OOM killer takes now fails the run rather than passing quietly.
# shellcheck source=tools/lib/jobs.sh
source "$ROOT_DIR/tools/lib/jobs.sh"
if [[ -z "$JOBS" ]]; then
  JOBS=$(dsd_default_jobs 1024)
  dsd_report_jobs clang-tidy 1024 "$JOBS"
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid jobs value: $JOBS" >&2
  exit 2
fi

# Prefer compile_commands from the dev-debug preset; otherwise, use top-level if present.
PDB_DIR="build/dev-debug"
PDB_FILE="$PDB_DIR/compile_commands.json"
if [ ! -f "$PDB_FILE" ]; then
  if [ -f "compile_commands.json" ]; then
    PDB_DIR="."
  else
    echo "Configuring CMake preset 'dev-debug' to generate compile_commands.json..."
    cmake --preset dev-debug > /dev/null
  fi
fi

# Collect translation units from the compilation database (excluding build and third-party).
# clang-tidy may process a file multiple times if it has multiple compile commands.
PDB_FILE="$PDB_DIR/compile_commands.json"
TIDY_PDB_DIR="$PDB_DIR"
TIDY_PDB_TEMP_DIR=""
if command -v python3 > /dev/null 2>&1; then
  # Always analyze against a rewritten database, --all-commands included: the
  # options clang cannot parse have to come out either way (see GCC_ONLY_ARGS
  # below), or every Qt translation unit aborts unanalyzed. --all-commands only
  # changes how many commands per file survive the rewrite, not whether one
  # happens.
  TIDY_PDB_TEMP_DIR=$(mktemp -d 2> /dev/null || mktemp -d -t dsd-neo-clang-tidy)
  trap 'rm -rf "$TIDY_PDB_TEMP_DIR" 2>/dev/null || true' EXIT
  TIDY_PDB_DIR="$TIDY_PDB_TEMP_DIR"

  # The rewritten database and the file list come from the same python run;
  # a process substitution would hide its exit status behind mapfile's, and an
  # empty list then reads as "nothing to analyze" and exits 0.
  TIDY_TU_LIST="$TIDY_PDB_TEMP_DIR/translation-units.txt"
  TU_LIST_RC=0
  python3 - "$PDB_FILE" "$ROOT_DIR" "$TIDY_PDB_DIR" "$ALL_COMMANDS" "${REQUESTED_FILES[@]}" << 'PY' > "$TIDY_TU_LIST" || TU_LIST_RC=$?
import json
import pathlib
import shlex
import sys

pdb_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2]).resolve()
out_dir = pathlib.Path(sys.argv[3]) if sys.argv[3] else None
all_commands = bool(int(sys.argv[4]))
requested = sys.argv[5:]

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

    return str(rel), str(file_path)


entries_by_rel = {}
total_entries = 0
for entry in data:
    rel, abs_path = normalize_file(entry)
    if not rel:
        continue
    total_entries += 1
    entries_by_rel.setdefault(rel, []).append(entry)

unique_files = sorted(entries_by_rel.keys())
multi_cmd_files = sum(1 for cmds in entries_by_rel.values() if len(cmds) > 1)
extra_cmds = sum(len(cmds) - 1 for cmds in entries_by_rel.values() if len(cmds) > 1)

print(
    f"Compilation database entries: {total_entries} (unique files: {len(unique_files)}, "
    f"files with multiple commands: {multi_cmd_files}, extra commands: {extra_cmds})",
    file=sys.stderr,
)


# Compiler options GCC accepts and clang rejects outright. clang-tidy parses the
# recorded command line, so one of these anywhere in it aborts the whole
# translation unit with clang-diagnostic-error and the file goes unanalyzed.
# Qt6 puts -mno-direct-extern-access on everything linking Qt when Qt was built
# with GCC, which is every Qt frontend and Qt test target in this tree.
# Dropping the flag changes nothing clang-tidy examines: it is a codegen option,
# and clang-tidy does not generate code.
# Keep in step with tools/iwyu.sh, which strips the same set.
GCC_ONLY_ARGS = frozenset({"-mno-direct-extern-access"})


def strip_gcc_only_args(entry):
    """Return a copy of entry with options clang cannot parse removed."""
    out = dict(entry)
    args = out.get("arguments")
    if args:
        out["arguments"] = [a for a in args if a not in GCC_ONLY_ARGS]
        return out
    cmd = out.get("command")
    if cmd:
        try:
            tokens = shlex.split(cmd)
        except Exception:
            return out
        kept = [t for t in tokens if t not in GCC_ONLY_ARGS]
        if len(kept) != len(tokens):
            out["command"] = shlex.join(kept)
    return out


def score_entry(rel_path, entry):
    cmd = entry.get("command") or ""
    args = entry.get("arguments")
    tokens = []
    if args:
        tokens = list(args)
    elif cmd:
        try:
            tokens = shlex.split(cmd)
        except Exception:
            tokens = cmd.split()

    score = 0
    # Prefer non-test compile commands when available.
    if any("/tests/" in t or t.startswith("tests/") for t in tokens):
        score -= 1000
    if any("tests/" in t for t in tokens if t.startswith("-I")):
        score -= 250
    # Prefer shared-library/object build flags.
    if "-fPIC" in tokens:
        score += 50
    if "-fPIE" in tokens:
        score -= 10
    # Prefer commands with more explicit defines/includes (tends to match real builds).
    score += sum(1 for t in tokens if t.startswith("-D"))
    score += sum(1 for t in tokens if t.startswith("-I"))
    # Tiebreaker: longer command line tends to be more complete.
    score += min(len(tokens), 500) // 10
    return score


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


requested_rel = []
if requested:
    seen = set()
    for p in requested:
        rel = normalize_requested(p)
        if not rel or rel in seen:
            continue
        seen.add(rel)
        requested_rel.append(rel)


def is_translation_unit(path):
    suffix = pathlib.Path(path).suffix.lower()
    return suffix in {".c", ".cc", ".cpp", ".cxx"}


selected_files = unique_files
if requested_rel:
    requested_tus = [p for p in requested_rel if is_translation_unit(p)]
    missing = [p for p in requested_tus if p not in entries_by_rel]
    if missing:
        # NOTE, not a bare message: this is the run analyzing less than it was
        # asked to, and in a lane the plain line went to a log nobody reads.
        # The names go on the marked line itself, capped: the gate collects the
        # line that carries the marker, so anything on a continuation line is
        # dropped and the note arrives with a dangling colon.
        shown = ", ".join(missing[:5])
        if len(missing) > 5:
            shown += f", and {len(missing) - 5} more"
        print(
            f"clang-tidy: NOTE: {len(missing)} requested file(s) are not in the "
            "compilation database and were not analyzed (it may be stale; "
            f"reconfigure the dev-debug preset): {shown}",
            file=sys.stderr,
        )
    selected_files = sorted(p for p in requested_tus if p in entries_by_rel)


if out_dir:
    out_dir.mkdir(parents=True, exist_ok=True)
    selected = []
    for rel in selected_files:
        cmds = entries_by_rel.get(rel)
        if not cmds:
            continue
        if all_commands:
            selected.extend(strip_gcc_only_args(entry) for entry in cmds)
        else:
            best = max(cmds, key=lambda e: score_entry(rel, e))
            selected.append(strip_gcc_only_args(best))

    out_path = out_dir / "compile_commands.json"
    out_path.write_text(json.dumps(selected, indent=2, sort_keys=True) + "\n")
    kind = "stripped" if all_commands else "deduped"
    print(f"Wrote {kind} compile database: {out_path} ({len(selected)} entries)", file=sys.stderr)

for path in selected_files:
    print(path)
PY
  if [[ $TU_LIST_RC -ne 0 ]]; then
    echo "clang-tidy: failed to build the translation-unit list from $PDB_FILE (python exited ${TU_LIST_RC})." >&2
    exit 1
  fi
  mapfile -t FILES < "$TIDY_TU_LIST"
else
  if [[ ${#REQUESTED_FILES[@]} -gt 0 ]]; then
    echo "python3 not found; analyzing requested files without compilation database filtering." >&2
    FILES=()
    for f in "${REQUESTED_FILES[@]}"; do
      f="${f#./}"
      case "$f" in
        build/* | src/third_party/*) continue ;;
      esac
      case "$f" in
        *.c | *.cc | *.cpp | *.cxx) FILES+=("$f") ;;
      esac
    done
  else
    echo "python3 not found; falling back to git ls-files (may include files not in compile database)." >&2
    mapfile -t FILES < <(git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' ':!:build/**' ':!:src/third_party/**')
  fi
fi
if [ ${#FILES[@]} -eq 0 ]; then
  if [[ ${#REQUESTED_FILES[@]} -gt 0 ]]; then
    echo "clang-tidy: NOTE: none of the requested paths are in the compilation database; nothing was analyzed."
  else
    echo "No source files found to analyze."
  fi
  exit 0
fi

echo "Using compilation database: $TIDY_PDB_DIR"
echo "Analyzing ${#FILES[@]} files with clang-tidy (${JOBS} at a time)..."
echo "clang-tidy version:"
clang-tidy --version | sed -n '1,2p'

# Run clang-tidy with project config and capture output
LOG_FILE=".clang-tidy.local.out"
CONFIG_FILE=".clang-tidy"

if [[ -f "$CONFIG_FILE" ]]; then
  CFG_PATH=$(readlink -f "$CONFIG_FILE" 2> /dev/null || echo "$CONFIG_FILE")
  echo "Using config file: $CFG_PATH"
else
  echo "Config file not found: $CONFIG_FILE (clang-tidy will use built-in defaults)"
fi

EXPECT_SUMMARY=0
FALLBACK_RC=0
if command -v python3 > /dev/null 2>&1; then
  # One clang-tidy process per translation unit, JOBS at a time. A single
  # clang-tidy invocation works through its files one after another on one
  # core, which is where the wall-clock time of a large change goes.
  EXPECT_SUMMARY=1
  python3 - "$TIDY_PDB_DIR" "$CONFIG_FILE" "$LOG_FILE" "$JOBS" "${FILES[@]}" << 'PY'
import concurrent.futures
import os
import subprocess
import sys

pdb_dir, config_file, log_file, jobs = sys.argv[1:5]
files = sys.argv[5:]
jobs = max(1, int(jobs))


def run_one(rel):
    cmd = ["clang-tidy", "-p", pdb_dir, "--config-file", config_file, rel]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    except Exception as exc:
        return rel, f"Failed to run clang-tidy on {rel}: {exc}", 1
    return rel, proc.stdout, proc.returncode


def size_of(rel):
    try:
        return os.path.getsize(rel)
    except OSError:
        return 0


# Largest files first, so the slowest units do not end up alone at the tail.
order = sorted(files, key=size_of, reverse=True)
analyzed = 0
unexplained = 0
# The log is opened before the first process starts and every section is
# flushed as it lands, so it can be tailed while the run is going and an
# interrupted run leaves a short log of this run rather than a complete-looking
# log of the last one. Sections are in completion order; the verdict below
# greps, so the order does not reach it.
with open(log_file, "w", encoding="utf-8") as log:
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_one, rel) for rel in order]
        # Ctrl-C otherwise runs the whole queue out: leaving the executor's
        # context waits for every already-submitted unit, which on a whole-tree
        # run is minutes of work after the interrupt. Cancelling the pending
        # futures leaves only the units already in flight, and re-raising skips
        # the summary line below so the verdict sees an unfinished run.
        try:
            for future in concurrent.futures.as_completed(futures):
                rel, output, rc = future.result()
                analyzed += 1
                log.write(f"===== clang-tidy: {rel} =====\n")
                log.write(output)
                if output and not output.endswith("\n"):
                    log.write("\n")
                # A worker killed without saying why - the OOM killer at a few
                # hundred MB per process, or a crash - would otherwise leave an
                # empty section, which reads exactly like a clean translation
                # unit.
                if rc != 0 and "error:" not in output:
                    unexplained += 1
                    log.write(
                        f"Error while processing {rel} (clang-tidy exited with status {rc} "
                        "and no diagnostics; the translation unit was not analyzed).\n"
                    )
                log.flush()
                print(f"[{analyzed}/{len(order)}] {rel}", file=sys.stderr)
        except KeyboardInterrupt:
            for future in futures:
                future.cancel()
            log.write("clang-tidy: interrupted; the remaining translation units were not analyzed.\n")
            log.flush()
            raise
    log.write(
        f"clang-tidy summary: analyzed={analyzed} of {len(order)} unexplained-failures={unexplained}\n"
    )
PY
else
  # No summary line to check here, so the one process's exit status is the only
  # evidence that it finished: `|| true` alone let a killed or crashed run reach
  # the greps below with a silent log and pass.
  set +e
  clang-tidy -p "$TIDY_PDB_DIR" --config-file "$CONFIG_FILE" "${FILES[@]}" 2>&1 | tee "$LOG_FILE" > /dev/null
  FALLBACK_RC=${PIPESTATUS[0]}
  set -e
fi

# Fail on error diagnostics (WarningsAsErrors) and on clang-tidy processing failures.
if rg -n "error:" "$LOG_FILE" > /dev/null; then
  echo "clang-tidy emitted diagnostics treated as errors. See $LOG_FILE for details." >&2
  echo "Summary (errors by check):" >&2
  # A header diagnostic now appears once per translation unit that includes
  # the header; count each distinct diagnostic once.
  rg --no-line-number "error:.*\\[[^]]+\\]$" "$LOG_FILE" | sort -u | sed -E 's/.*\[([^]]+)\]$/\1/' | awk -F',' '{print $1}' | sort | uniq -c | sort -nr >&2
  exit 1
fi
if rg -n "^Error while processing " "$LOG_FILE" > /dev/null; then
  echo "clang-tidy failed to process one or more files. See $LOG_FILE for details." >&2
  rg -n "^Error while processing " "$LOG_FILE" >&2 || true
  exit 1
fi
# The run has to have reached every translation unit it was given: a killed
# worker or an interrupted run leaves a log that is silent rather than wrong.
if [[ $EXPECT_SUMMARY -eq 1 ]]; then
  if ! rg -n "^clang-tidy summary: analyzed=${#FILES[@]} of ${#FILES[@]} " "$LOG_FILE" > /dev/null; then
    echo "clang-tidy did not analyze all ${#FILES[@]} translation unit(s). See $LOG_FILE for details." >&2
    rg -n "^clang-tidy summary: " "$LOG_FILE" >&2 || echo "clang-tidy: the run did not finish (no summary line)." >&2
    exit 1
  fi
elif [[ $FALLBACK_RC -ne 0 ]]; then
  # The greps above already exited for real diagnostics, so a non-zero status
  # here means the process died without saying why.
  echo "clang-tidy exited with status ${FALLBACK_RC} and emitted no diagnostics; the run did not finish. See $LOG_FILE for details." >&2
  exit 1
fi

echo "clang-tidy clean for error diagnostics. Full output in $LOG_FILE"
