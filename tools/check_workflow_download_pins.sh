#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$repo_root"

if ! command -v rg > /dev/null 2>&1; then
  echo "ripgrep is required for workflow download pin guardrail checks." >&2
  exit 2
fi

failed=0

report_violation() {
  local title=$1
  local body=$2
  if [[ -n "$body" ]]; then
    echo "$title" >&2
    printf '%s\n' "$body" >&2
    failed=1
  fi
}

continuous_downloads=$(
  rg -n 'releases/download/continuous' .github/workflows tools \
    --glob '!tools/check_workflow_download_pins.sh' ||
    true
)
report_violation "Mutable GitHub release download URL detected:" "$continuous_downloads"

appimage_downloads=$(
  rg -n 'wget[^\n]*(AppImage|linuxdeploy)' .github/workflows tools \
    --glob '!tools/check_workflow_download_pins.sh' ||
    true
)
report_violation "Downloaded executable AppImage helper detected; build helper tools from pinned source instead:" \
  "$appimage_downloads"

floating_appimage_images=$(
  rg -n '(ubuntu:20[.]04|public[.]ecr[.]aws/lts/ubuntu:20[.]04)' .github/workflows tools/ci-dependency-pins.env \
    --glob '!tools/check_workflow_download_pins.sh' |
    grep -v '@sha256:' ||
    true
)
report_violation "Digestless AppImage container image reference detected:" "$floating_appimage_images"

floating_ci_images=$(
  rg -n '(archlinux:base-devel|tonistiigi/binfmt|ghcr[.]io/(gitleaks/gitleaks|google/osv-scanner):)' \
    .github/workflows tools \
    --glob '!tools/check_workflow_download_pins.sh' |
    grep -v '@sha256:' ||
    true
)
report_violation "Digestless CI container image reference detected:" "$floating_ci_images"

floating_install_images=$(
  rg -n '(ubuntu:(24|26)[.]04|debian:(12|13)|fedora:44|rockylinux:9|almalinux:9|quay[.]io/centos/centos:stream9|opensuse/(leap:15[.]6|tumbleweed:latest)|alpine:3[.]24)' \
    .github/workflows tools \
    --glob '!tools/check_workflow_download_pins.sh' |
    grep -v '@sha256:' ||
    true
)
report_violation "Digestless Linux install-matrix container image reference detected:" "$floating_install_images"

# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source tools/ci-dependency-pins.env

for var in \
  APPIMAGE_UBUNTU_2004_AMD64_IMAGE \
  APPIMAGE_UBUNTU_2004_ARM64_IMAGE \
  ARCHLINUX_BASE_DEVEL_IMAGE \
  INSTALL_UBUNTU_2604_IMAGE \
  INSTALL_UBUNTU_2404_IMAGE \
  INSTALL_DEBIAN_13_IMAGE \
  INSTALL_DEBIAN_12_IMAGE \
  INSTALL_FEDORA_44_IMAGE \
  INSTALL_ROCKY_9_IMAGE \
  INSTALL_ALMALINUX_9_IMAGE \
  INSTALL_CENTOS_STREAM_9_IMAGE \
  INSTALL_OPENSUSE_LEAP_160_IMAGE \
  INSTALL_OPENSUSE_TUMBLEWEED_IMAGE \
  INSTALL_ALPINE_324_IMAGE \
  TONISTIIGI_BINFMT_IMAGE \
  GITLEAKS_IMAGE \
  OSV_SCANNER_IMAGE; do
  value=${!var:-}
  if [[ ! "$value" =~ @sha256:[0-9a-f]{64}$ ]]; then
    echo "${var} must be pinned as image@sha256:<64 hex chars>; got '${value}'." >&2
    failed=1
  fi
done

# The jobs that run in the Arch image name it inline, because a container image
# cannot read the env file, so each copy has to equal ARCHLINUX_BASE_DEVEL_IMAGE.
inline_arch_images=$(
  rg -n -o 'archlinux:base-devel@sha256:[0-9a-f]+' .github/workflows --glob '*.y*ml' |
    grep -vF ":${ARCHLINUX_BASE_DEVEL_IMAGE:-}" ||
    true
)
report_violation "Inline archlinux:base-devel image differs from ARCHLINUX_BASE_DEVEL_IMAGE in tools/ci-dependency-pins.env:" \
  "$inline_arch_images"

# A job container or service that pulls with `credentials:` cannot start where the
# secrets behind them are empty: GitHub rejects an empty username or password before
# the job runs. Dependabot and fork PRs get no Docker Hub secrets, so after #665 every
# Dependabot PR failed the two Arch jobs at "Set up job". In a workflow that runs on
# pull requests, such a job must keep itself off them: `github.event_name !=
# 'pull_request'` has to be one of the &&-joined terms at the top of its job-level
# condition, so no other branch of the expression can let a pull request through. A
# job that has to run there pulls in a step (tools/ci_arch_toolchain.sh) after an
# optional login.
if ! command -v python3 > /dev/null 2>&1; then
  echo "python3 is required for workflow download pin guardrail checks." >&2
  exit 2
fi
pr_job_credentials=$(
  python3 - << 'PY'
import pathlib
import re

KEY = re.compile(r"""^(?:"([^"]*)"|'([^']*)'|([^\s"'#][^:#]*?))\s*:(?:\s+(.*))?$""")
EXCLUSION = "github.event_name != 'pull_request'"


def strip_comment(line):
    """Drop a YAML comment: a '#' at the start or after a blank, outside quotes."""
    quote = None
    for i, ch in enumerate(line):
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i].rstrip()
    return line.rstrip()


def entries(text):
    """(line number, indent, key, value) for each mapping-key line; None key otherwise."""
    for number, raw in enumerate(text.splitlines(), 1):
        line = strip_comment(raw.replace("\t", " "))
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        match = KEY.match(line.strip())
        if match:
            key = next(g for g in match.groups()[:3] if g is not None)
            yield number, indent, key.strip(), (match.group(4) or "").strip()
        else:
            yield number, indent, None, line.strip()


def unwrap(expr):
    """Strip parentheses that enclose the whole expression."""
    while expr.startswith("(") and expr.endswith(")"):
        depth = 0
        quote = None
        for i, ch in enumerate(expr):
            if quote:
                quote = None if ch == quote else quote
            elif ch == "'":
                quote = ch
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and i != len(expr) - 1:
                    return expr
        expr = expr[1:-1].strip()
    return expr


def excludes_pull_requests(condition):
    expr = re.sub(r"^[|>][+-]?\s*", "", condition.strip())
    if len(expr) >= 2 and expr[0] == expr[-1] and expr[0] in "'\"":
        expr = expr[1:-1].replace("''", "'") if expr[0] == "'" else expr[1:-1]
    expr = expr.strip()
    if expr.startswith("${{") and expr.endswith("}}"):
        expr = expr[3:-2].strip()
    expr = unwrap(expr)
    terms = []
    depth = 0
    quote = None
    current = ""
    i = 0
    while i < len(expr):
        ch = expr[i]
        if quote:
            quote = None if ch == quote else quote
        elif ch == "'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and expr.startswith("||", i):
            return False
        elif depth == 0 and expr.startswith("&&", i):
            terms.append(current)
            current = ""
            i += 2
            continue
        current += ch
        i += 1
    terms.append(current)
    return any(" ".join(unwrap(t.strip()).split()) == EXCLUSION for t in terms)


for workflow in sorted(pathlib.Path(".github/workflows").glob("*.y*ml")):
    rows = list(entries(workflow.read_text(encoding="utf-8")))
    on_text = []
    section = None
    for _, indent, key, value in rows:
        if indent == 0:
            section = key
            if key in ("on", "true"):
                on_text.append(value)
            continue
        if section in ("on", "true"):
            on_text.append(f"{key or ''} {value}")
    if not re.search(r"\bpull_request(_target)?\b", " ".join(on_text)):
        continue

    section = None
    job_indent = field_indent = None
    job = job_line = None
    field = None
    condition = []
    credentials_line = None

    def finish():
        if job is not None and credentials_line is not None and not excludes_pull_requests(" ".join(condition)):
            print(f"{workflow}:{credentials_line}: job {job}")

    for number, indent, key, value in rows:
        if indent == 0:
            finish()
            section, job = key, None
            continue
        if section != "jobs":
            continue
        if job_indent is None:
            job_indent = indent
        if indent <= job_indent:
            finish()
            job, field, condition, credentials_line, field_indent = key, None, [], None, None
            continue
        if job is None:
            continue
        if field_indent is None:
            field_indent = indent
        if indent == field_indent:
            field = key
            if key == "if":
                condition = [value]
            continue
        if field == "if":
            condition.append(value if key is None else f"{key}: {value}")
        elif field in ("container", "services") and key == "credentials" and credentials_line is None:
            credentials_line = number
    finish()
PY
)
report_violation "Job pulls its container with credentials yet can run on a pull request, where Dependabot and forks have no secrets:" \
  "$pr_job_credentials"

for var in CMAKE_LINUX_X86_64_SHA256 CMAKE_LINUX_AARCH64_SHA256 KOTLIN_COMPILER_SHA256; do
  value=${!var:-}
  if [[ ! "$value" =~ ^[0-9a-f]{64}$ ]]; then
    echo "${var} must be a SHA256 digest; got '${value}'." >&2
    failed=1
  fi
done

if rg -q 'cmake-[0-9][^[:space:]]+-linux-[^[:space:]]+[.]sh' .github/workflows/linux-appimage.yaml &&
  ! rg -q 'sha256sum[[:space:]]+-c[[:space:]]+-' .github/workflows/linux-appimage.yaml; then
  echo "CMake installer download in linux-appimage workflow must be verified with sha256sum -c - before execution." >&2
  failed=1
fi

# A workflow that downloads the Kotlin compiler must check the archive against
# KOTLIN_COMPILER_SHA256 with sha256sum -c - before it unpacks and runs it.
while IFS= read -r workflow; do
  if ! rg -q 'KOTLIN_COMPILER_SHA256[^|]*[|][[:space:]]*sha256sum[[:space:]]+-c[[:space:]]+-' "$workflow"; then
    echo "Kotlin compiler download in $workflow must be verified against KOTLIN_COMPILER_SHA256 with sha256sum -c -." >&2
    failed=1
  fi
done < <(rg -l 'kotlin-compiler-' .github/workflows --glob '*.y*ml' || true)

exit "$failed"
