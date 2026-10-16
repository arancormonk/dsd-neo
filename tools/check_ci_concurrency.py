#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the shared runner budget in the repository's block-style workflows.

actionlint validates YAML and expressions. This check additionally requires every
runner job to use one of our 18 slots, with a non-replacing queue. Keep its small
reader deliberately strict: unsupported job/concurrency/matrix layouts must fail,
not silently escape the budget. No third-party Python packages are needed in CI.
"""

import argparse
import pathlib
import re
import sys

GROUPS = frozenset(
    [f"ci-worker-{i:02d}" for i in range(1, 14)]
    + ["ci-fast-1", "ci-fast-2", "ci-control", "aur-dsd-neo", "aur-dsd-neo-git"]
)
CHANGES_WORKFLOW = "./.github/workflows/ci-changes.yml"


def scalar(value):
    return value.strip().strip("\"'")


def block(text, key, indent):
    """Read one consistently indented mapping, refusing duplicate/flow keys."""
    lines = text.splitlines()
    starts = [
        i
        for i, line in enumerate(lines)
        if re.match(rf"^{' ' * indent}{re.escape(key)}:", line)
    ]
    if len(starts) != 1 or lines[starts[0]].strip() != f"{key}:":
        raise ValueError(f"expected one block-style {key} mapping")
    result = []
    for line in lines[starts[0] + 1 :]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if len(line) - len(line.lstrip()) <= indent:
            break
        result.append(line)
    return "\n".join(result)


def check_workflow(text):
    jobs = block(text, "jobs", 0)
    matches = list(re.finditer(r"^  ([a-zA-Z0-9_-]+):\s*$", jobs, re.MULTILINE))
    declarations = [
        line
        for line in jobs.splitlines()
        if line.startswith("  ") and not line.startswith("   ")
    ]
    if not matches or len(matches) != len(declarations):
        raise ValueError("jobs must have plain, block-style job IDs")
    used = set()
    names = set()
    for index, match in enumerate(matches):
        name = match.group(1)
        if name in names:
            raise ValueError(f"duplicate job {name}")
        names.add(name)
        end = matches[index + 1].start() if index + 1 < len(matches) else len(jobs)
        job = jobs[match.end() : end]
        if re.search(r"^    <<:", job, re.MULTILINE):
            raise ValueError(f"{name}: merged job mappings are not supported")
        runner = re.findall(r"^    runs-on:", job, re.MULTILINE)
        if not runner:
            calls = re.findall(r"^    uses: (\S+)(?:\s+#.*)?$", job, re.MULTILINE)
            if calls != [CHANGES_WORKFLOW]:
                raise ValueError(
                    f"{name}: only the budgeted changes workflow may be called"
                )
            continue
        if len(runner) != 1:
            raise ValueError(f"{name}: expected one runner declaration")
        concurrency = block(job, "concurrency", 4)
        settings = {}
        for line in concurrency.splitlines():
            setting = re.fullmatch(
                r"      (group|cancel-in-progress|queue): (.+)", line
            )
            if not setting or setting.group(1) in settings:
                raise ValueError(
                    f"{name}: unsupported concurrency setting: {line.strip()}"
                )
            settings[setting.group(1)] = scalar(setting.group(2))
        if (
            settings.get("queue") != "max"
            or settings.get("cancel-in-progress") != "false"
        ):
            raise ValueError(
                f"{name}: slots require queue: max and cancel-in-progress: false"
            )
        group = settings.get("group")
        groups = {group}
        if group == "${{ matrix.ci_group }}":
            if re.search(r"^    needs: changes$", job, re.MULTILINE):
                conditions = re.findall(r"^    if: (.+)$", job, re.MULTILINE)
                if conditions != ["${{ !cancelled() }}"]:
                    raise ValueError(
                        f"{name}: classified matrices must expand to report all required checks"
                    )
            matrix = block(block(job, "strategy", 4), "matrix", 6)
            include = block(matrix, "include", 8)
            if any(
                line.startswith("        ")
                and not line.startswith("         ")
                and line.strip() != "include:"
                for line in matrix.splitlines()
            ):
                raise ValueError(f"{name}: budgeted matrices must use include only")
            entries = re.split(r"^          - ", include, flags=re.MULTILINE)[1:]
            if not entries:
                raise ValueError(f"{name}: empty budgeted matrix")
            groups = set()
            for entry in entries:
                slots = re.findall(r"^            ci_group: (.+)$", entry, re.MULTILINE)
                if len(slots) != 1:
                    raise ValueError(
                        f"{name}: every matrix entry needs exactly one ci_group"
                    )
                groups.add(scalar(slots[0]))
        if not groups <= GROUPS:
            raise ValueError(f"{name}: unbudgeted groups: {groups - GROUPS}")
        used.update(groups)
    return used


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    paths = args.files or sorted(pathlib.Path(".github/workflows").glob("*.y*ml"))
    used = set()
    for path in paths:
        try:
            used.update(check_workflow(path.read_text(encoding="utf-8")))
        except (OSError, ValueError) as error:
            print(f"{path}: {error}", file=sys.stderr)
            return 1
    if not paths:
        print("No workflows found", file=sys.stderr)
        return 1
    print(f"CI runner budget: {len(used)} shared slots (maximum {len(GROUPS)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
