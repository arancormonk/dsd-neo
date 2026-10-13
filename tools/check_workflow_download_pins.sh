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
# Dependabot PR failed the two Arch jobs at "Set up job". Such a job must keep itself
# off every pull-request event its workflow receives (pull_request,
# pull_request_target, and both for a reusable workflow, which takes its caller's
# event): each `github.event_name != '<event>'`, or a `github.event_name ==` another
# event, has to be one of the &&-joined terms at the top of its job-level condition,
# so no other branch of the expression lets a pull request through. A job that has to
# run there pulls in a step (tools/ci_arch_toolchain.sh) after an optional login.
#
# The check reads the workflow line by line rather than through a YAML library, which
# the hosts that run it do not all have. That is sound for block style, where
# indentation alone gives the structure. What it cannot read that way (a top level
# not in block style, a flow collection spread over lines, a job written in flow
# style, aliases or merge keys) fails closed whenever it could hide credentials:
# write such a workflow out in block style. A condition that is always true because
# of text around its ${{ }} (a trailing newline from `if: |`, say) is actionlint's
# if-cond error, which the required shell/workflow lint check rejects; this check
# does not model it.
if ! command -v python3 > /dev/null 2>&1; then
  echo "python3 is required for workflow download pin guardrail checks." >&2
  exit 2
fi
pr_job_credentials=$(
  python3 - << 'PY'
import pathlib
import re

KEY = re.compile(r"""^(?:"([^"]*)"|'([^']*)'|([^\s"'#\-{[*&!][^:#]*?))\s*:(?:\s+(.*))?$""")
CREDENTIALS = re.compile(r"""(?:^|[\s{,])["']?credentials["']?\s*:""")
QUOTED = re.compile(r"'(?:[^']|'')*'" + r'|"(?:[^"\\]|\\.)*"')
FLOW_ALIAS = re.compile(r"""[{,\[:]\s*\*|<<\s*:""")
PR_EVENTS = ("pull_request", "pull_request_target")
FLOW = ("{", "[")
BLOCK_HEADER = re.compile(r"^[|>](?:[+-]?[1-9]?|[1-9][+-])(?:\s+|$)")


def strip_comment(line):
    """Drop a YAML comment: a '#' at the start or after a blank, outside quotes."""
    quote = None
    escaped = False
    for i, ch in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif quote == '"' and ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i].rstrip()
    return line.rstrip()


# A tagged or anchored key (!!str jobs:, &a key:), an explicit "? " key, or a
# double-quoted key with an escape, at the start of a line or inside a flow
# collection (outside its ${{ }} expressions) ("con\u0074ainer") or an explicit "? " key
# could spell any key, so the reader does not try to decode it.
ODD_KEY_LINE = re.compile(r"""^(?:-\s+)*(?:\?(?:\s|$)|[!&]\S+\s+[^\s:,{}\[\]][^:,{}]*:|"[^"]*\\[^"]*"\s*:)""")
ODD_KEY_FLOW = re.compile(r"""[{,\[]\s*(?:\?(?:\s|$)|[!&]\S+\s+[^\s:,{}\[\]][^:,{}]*:|"[^"]*\\[^"]*"\s*:)""")
EXPRESSION = re.compile(r"\$\{\{.*?\}\}")


def closes(text, quote):
    """True when a quoted scalar's closing quote is in text."""
    i = 0
    while i < len(text):
        ch = text[i]
        if quote == '"' and ch == "\\":
            i += 2
            continue
        if ch == quote:
            if quote == "'" and text[i + 1 : i + 2] == "'":
                i += 2
                continue
            return True
        i += 1
    return False


def entries(text, odd_keys, content):
    """(line number, indent, key, value, text) per line; key is None for a non-key line.

    The lines inside a block scalar (key: | or >) or a quoted scalar spanning lines
    come back as non-key lines with their raw text, and their numbers go into
    content: there a '#' is content, not a comment, and nothing is a key."""
    block_indent = None
    quote_open = None
    for number, raw in enumerate(text.splitlines(), 1):
        expanded = raw.replace("\t", " ")
        if not expanded.strip():
            continue
        indent = len(expanded) - len(expanded.lstrip(" "))
        if quote_open is not None:
            content.add(number)
            yield number, indent, None, expanded.strip(), expanded.strip()
            if closes(expanded.strip(), quote_open):
                quote_open = None
            continue
        if block_indent is not None:
            if indent > block_indent:
                content.add(number)
                yield number, indent, None, expanded.strip(), expanded.strip()
                continue
            block_indent = None
        line = strip_comment(expanded)
        if not line.strip():
            continue
        body = line.strip()
        # The column of the key (past any "- " of a list item) bounds the scalar.
        item = re.match(r"^(?:-\s+)*", body).end()
        match = KEY.match(body[item:])
        if match:
            value = (match.group(4) or "").strip()
            if BLOCK_HEADER.match(value):
                block_indent = indent + item
        scalar = value if match else body[item:]
        if ODD_KEY_LINE.match(body) or (
            scalar.startswith(FLOW) and ODD_KEY_FLOW.search(EXPRESSION.sub("", scalar))
        ):
            odd_keys.append(number)
        if scalar[:1] in ("'", '"') and not closes(scalar[1:], scalar[0]):
            quote_open = scalar[0]
        if match and not item:
            key = next(g for g in match.groups()[:3] if g is not None)
            yield number, indent, key.strip(), value, body
        else:
            yield number, indent, None, body, body


def unbalanced(text):
    """True when this value is a flow collection ({...} or [...]) that does not
    close on the line it opens. Braces inside a plain scalar, such as an
    expression's ${{ }}, are not a flow collection."""
    text = re.sub(r"^(?:-\s+)*", "", text.strip())
    text = re.sub(r"^(?:[&!]\S+\s*)+", "", text)
    if not text.startswith(FLOW):
        return False
    depth = 0
    quote = None
    for ch in text:
        if quote:
            quote = None if ch == quote else quote
        elif ch in "'\"":
            quote = ch
        elif ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
    return depth != 0


def node_alias(text):
    """True when this node is an alias (*name), or a flow collection holding an
    alias or a merge key outside its quoted strings. A '*' inside a scalar, such as
    a glob, is not an alias."""
    text = re.sub(r"^(?:-\s+)*", "", text.strip())
    text = re.sub(r"^(?:[&!]\S+\s*)+", "", text)
    if text.startswith("*"):
        return True
    if text.startswith(FLOW):
        return bool(FLOW_ALIAS.search(QUOTED.sub('""', text)))
    return False


def drop_anchor(value):
    """A value without its leading anchor or tag, which leave the content below."""
    return re.sub(r"^(?:[&!]\S+\s*)+", "", value)


def unquote(token):
    token = token.strip()
    if len(token) >= 2 and token[0] == token[-1] and token[0] in "'\"":
        return token[1:-1]
    return token


def flow_names(text):
    """The items of a flow sequence, or the keys of a flow mapping."""
    items = []
    depth = 0
    quote = None
    item = ""
    for ch in text:
        if quote:
            quote = None if ch == quote else quote
        elif ch in "'\"":
            quote = ch
        elif ch in "[{":
            depth += 1
            if depth == 1:
                continue
        elif ch in "]}":
            depth -= 1
            if depth == 0:
                items.append(item)
                item = ""
                continue
        elif ch == "," and depth == 1:
            items.append(item)
            item = ""
            continue
        if depth >= 1:
            item += ch
    names = []
    for item in items:
        # The key ends at the first colon outside quotes; event names have none.
        quote = None
        key = item
        for i, ch in enumerate(item):
            if quote:
                quote = None if ch == quote else quote
            elif ch in "'\"":
                quote = ch
            elif ch == ":":
                key = item[:i]
                break
        if key.strip():
            names.append(unquote(key))
    return names


def trigger_names(rows):
    """The event names under the workflow's `on`, or None when they cannot be read."""
    on_rows = []
    in_on = False
    for row in rows:
        _, indent, key, value, body = row
        if indent == 0 and not body.startswith("-"):
            in_on = key in ("on", "true")
            if in_on:
                on_rows.append(row)
            continue
        if in_on:
            on_rows.append(row)
    # A flow collection spread over lines is not read: it counts as unreadable.
    if not on_rows or any(unbalanced(r[3] if r[2] is not None else r[4]) for r in on_rows):
        return None
    value = drop_anchor(on_rows[0][3])
    rest = on_rows[1:]
    if BLOCK_HEADER.match(value):
        return None
    if not value and rest and rest[0][4].startswith(FLOW):
        value, rest = rest[0][4], rest[1:]
    if value.startswith(FLOW):
        if rest:
            return None
        names = flow_names(value)
    elif value:
        names = [unquote(value)]
    else:
        names = []
        child_indent = None
        for _, indent, key, _, body in rest:
            if child_indent is None:
                child_indent = indent
            if indent != child_indent:
                continue
            if body.startswith("-"):
                names.append(unquote((flow_names("[" + body[1:] + "]") or [""])[0]))
            else:
                names.append(key if key is not None else unquote(body))
    names = [n.strip().lower() for n in names]
    # Event names are plain words; anything else (an escape sequence, an alias, a
    # merge key, a nested collection) is not read and so counts as unreadable.
    if not names or not all(re.fullmatch(r"[a-z_]+", n) for n in names):
        return None
    return names


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


def top_level_terms(condition, block):
    """The &&-joined terms at the top of a condition, lower-cased (GitHub compares
    names and strings case-insensitively), or None when it has a top-level ||. A
    block scalar's content is literal: no quotes are removed from it."""
    expr = condition.strip()
    if block:
        expr = BLOCK_HEADER.sub("", expr, count=1)
    elif expr.startswith('"') and "\\" in expr:
        # A double-quoted YAML scalar decodes escapes (\u005f is "_", \x7c is "|"),
        # which could hide an event name or an operator: not read.
        return None
    elif len(expr) >= 2 and expr[0] == expr[-1] and expr[0] in "'\"":
        expr = expr[1:-1].replace("''", "'") if expr[0] == "'" else expr[1:-1]
    expr = expr.strip()
    if "${{" in expr:
        # Only one expression around the whole condition evaluates as a condition:
        # GitHub reads text around or between ${{ }} as a string, always true.
        if not (expr.startswith("${{") and expr.endswith("}}") and expr.count("${{") == 1):
            return None
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
            return None
        elif depth == 0 and expr.startswith("&&", i):
            terms.append(current)
            current = ""
            i += 2
            continue
        current += ch
        i += 1
    terms.append(current)
    terms = [" ".join(unwrap(t.strip()).split()).lower() for t in terms]
    return [re.sub(r"\s*(==|!=)\s*", r" \1 ", t) for t in terms]


def excludes(condition, events, block):
    terms = top_level_terms(condition, block)
    if terms is None:
        return False
    for term in terms:
        match = re.fullmatch(r"github\.event_name == '([a-z_]+)'", term)
        if match and match.group(1) not in PR_EVENTS:
            return True
    return all(f"github.event_name != '{event}'" in terms for event in events)


class Job:
    """One job, read field by field, or as opaque text when it is not in block style."""

    def __init__(self, name, inline, number):
        self.name = name
        self.opaque = bool(inline)
        self.condition = []
        # A block scalar condition (if: >- or |) is literal: entries() keeps its raw
        # lines, and no quotes are removed. A quoted condition over several lines is
        # not read: its comment-like text cannot be told apart, so it never excludes.
        self.block = False
        self.condition_readable = True
        self.field = None
        self.field_indent = None
        self.credentials = None
        self.unreadable = None
        # Indentation of the container's keys, of the services, and of a service's keys.
        self.container_keys = None
        self.service_indent = None
        self.service_keys = None
        if inline:
            self.scan(inline, number)

    def scan(self, text, number):
        if self.unreadable is None and node_alias(text):
            self.unreadable = number
        if self.credentials is None and CREDENTIALS.search(text):
            self.credentials = number

    def continue_condition(self, text):
        """A further line of the job's condition."""
        if not self.block and self.condition[0][:1] in ("'", '"'):
            self.condition_readable = False
        self.condition.append(text)

    def found_credentials(self, number):
        if self.credentials is None:
            self.credentials = number

    def flow_credentials(self, value, number):
        """A one-line flow mapping ({image: ..., credentials: ...}) with credentials."""
        if value.startswith("{") and "credentials" in flow_names(value):
            self.found_credentials(number)

    def report(self, workflow, events):
        if self.unreadable is not None:
            print(f"{workflow}:{self.unreadable}: job {self.name} (aliases, merge keys or flow style; write it in block style)")
        elif self.credentials is not None and (
            self.opaque
            or not self.condition_readable
            or not excludes(" ".join(self.condition), events, self.block)
        ):
            print(f"{workflow}:{self.credentials}: job {self.name}")


for workflow in sorted(pathlib.Path(".github/workflows").glob("*.y*ml")):
    text = workflow.read_text(encoding="utf-8")
    odd_keys = []
    content = set()
    rows = list(entries(text, odd_keys, content))
    if odd_keys:
        print(f"{workflow}:{odd_keys[0]}: workflow (an escaped or explicit (?) key; write keys plainly)")
        continue
    # The reader relies on block style: keys at column 0 for the top level and
    # indentation for everything below. A workflow that is not written that way
    # fails whenever it mentions credentials at all.
    first = next((r for r in rows if r[4] != "---"), None)
    if first is None or first[1] != 0 or first[2] is None:
        if CREDENTIALS.search(text):
            line = first[0] if first else 1
            print(f"{workflow}:{line}: workflow (not block style at the top level; write it in block style)")
        continue
    triggers = trigger_names(rows)
    if triggers is None:
        # Triggers it cannot read count as pull requests: fail closed.
        events = set(PR_EVENTS)
    else:
        events = {e for e in PR_EVENTS if e in triggers}
        if "workflow_call" in triggers:
            events.update(PR_EVENTS)
    if not events:
        continue

    in_jobs = False
    job_indent = None
    job = None
    for number, indent, key, value, body in rows:
        if number in content:
            # Scalar content never opens a job or a field.
            if in_jobs and job is not None:
                if job.opaque:
                    job.scan(body, number)
                elif job.field == "if":
                    job.continue_condition(body)
            continue
        if indent == 0 and not body.startswith("-"):
            if job is not None:
                job.report(workflow, events)
            job = None
            in_jobs = key == "jobs"
            inline = drop_anchor(value) if in_jobs else ""
            if inline:
                # `jobs` written inline is read as one opaque job.
                job = Job("(jobs)", inline, number)
                job_indent = -1
            continue
        if not in_jobs:
            continue
        if job_indent is None:
            job_indent = indent
        if indent <= job_indent:
            if job is not None:
                job.report(workflow, events)
            if key is None:
                # Something other than a job key where one belongs.
                job = Job("(jobs)", body, number)
                job_indent = -1
            else:
                job = Job(key, drop_anchor(value), number)
            if unbalanced(value if key is not None else body) and job.unreadable is None:
                job.unreadable = number
            continue
        if job is None:
            continue
        if job.opaque:
            job.scan(body, number)
            continue
        if job.field_indent is None:
            if body.startswith(FLOW) or body.startswith("*"):
                # The job's mapping opens on the next line in flow style.
                job.opaque = True
                job.scan(body, number)
                continue
            job.field_indent = indent
        text = body
        if indent == job.field_indent:
            job.field = key
            text = value
            if unbalanced(value if key is not None else body) and job.unreadable is None:
                job.unreadable = number
            if key == "if":
                job.condition = [value]
                job.block = bool(BLOCK_HEADER.match(value))
            if key == "<<" and job.unreadable is None:
                job.unreadable = number
        elif job.field == "if":
            job.continue_condition(body)
        # Only a credentials key of the container itself, or of a service, counts:
        # the same word in an env value or a nested mapping is not registry auth.
        if job.field == "container":
            if indent == job.field_indent:
                job.container_keys = None
                job.flow_credentials(value, number)
            else:
                if job.container_keys is None:
                    job.container_keys = indent
                if indent == job.container_keys:
                    if key == "credentials":
                        job.found_credentials(number)
                    elif key is None:
                        # The container's mapping on the next line, in flow style.
                        job.flow_credentials(body, number)
        elif job.field == "services":
            if indent == job.field_indent:
                job.service_indent = job.service_keys = None
                if value.startswith("{"):
                    # Services and their settings on one line: any mention counts.
                    job.scan(value, number)
            else:
                if job.service_indent is None:
                    job.service_indent = indent
                if indent == job.service_indent:
                    job.service_keys = None
                    if key is None:
                        # The services and their settings on the next line: any
                        # mention counts.
                        job.scan(body, number)
                    else:
                        job.flow_credentials(value, number)
                elif job.service_keys is None or indent == job.service_keys:
                    job.service_keys = indent
                    if key == "credentials":
                        job.found_credentials(number)
                    elif key is None:
                        # A service's mapping on the next line, in flow style.
                        job.flow_credentials(body, number)
        if job.field in ("container", "services"):
            if job.unreadable is None and (key == "<<" or node_alias(value if key is not None else body)):
                job.unreadable = number
            if unbalanced(value if key is not None else body) and job.unreadable is None:
                job.unreadable = number
    if job is not None:
        job.report(workflow, events)
PY
)
report_violation "Job pulls its container with credentials yet can run on a pull request, where Dependabot and forks have no secrets (or cannot be read to tell):" \
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
