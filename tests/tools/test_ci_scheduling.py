#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise runner-budget escapes and PR cleanup without contacting GitHub."""

import copy
import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


budget = load("check_ci_concurrency")
cleanup = load("ci_cancel_closed_pr")
WORKFLOW = """name: test
on: workflow_dispatch
jobs:
  build:
    runs-on: ubuntu-latest
    concurrency:
      group: ci-worker-01
      cancel-in-progress: false
      queue: max
    steps:
      - run: echo checked
"""
PR = {
    "number": 12,
    "state": "closed",
    "created_at": "2026-10-01T10:00:00Z",
    "closed_at": "2026-10-01T11:00:00Z",
    "base": {"repo": {"id": 1}},
    "head": {"repo": {"id": 2}, "ref": "fix/branch"},
}
RUN = {
    "id": 34,
    "event": "pull_request",
    "status": "in_progress",
    "repository": {"id": 1},
    "head_repository": {"id": 2},
    "head_branch": "fix/branch",
    "created_at": "2026-10-01T10:30:00Z",
    "pull_requests": [],
}


class BudgetTests(unittest.TestCase):
    def test_repository_budget(self):
        used = set()
        for path in (ROOT / ".github/workflows").glob("*.y*ml"):
            with self.subTest(workflow=path.name):
                used.update(budget.check_workflow(path.read_text()))
        self.assertEqual(len(used), 18)

    def test_shared_slot(self):
        self.assertEqual(budget.check_workflow(WORKFLOW), {"ci-worker-01"})
        two = WORKFLOW + WORKFLOW[WORKFLOW.index("  build:") :].replace(
            "  build:", "  test:", 1
        )
        self.assertEqual(budget.check_workflow(two), {"ci-worker-01"})

    def test_ref_specific_or_extra_slots_are_rejected(self):
        for group in (
            "ci-worker-14",
            "ci-worker-${{ github.ref }}",
            "ci-worker-${{ github.run_id }}",
        ):
            with self.subTest(group=group), self.assertRaises(ValueError):
                budget.check_workflow(WORKFLOW.replace("ci-worker-01", group))

    def test_replacing_or_missing_queue_is_rejected(self):
        for replacement in ("      queue: single\n", "      queue: typo\n", ""):
            with self.subTest(queue=replacement), self.assertRaises(ValueError):
                budget.check_workflow(
                    WORKFLOW.replace("      queue: max\n", replacement)
                )
        with self.assertRaises(ValueError):
            budget.check_workflow(
                WORKFLOW.replace(
                    "cancel-in-progress: false", "cancel-in-progress: true"
                )
            )

    def test_unbudgeted_runner_and_external_workflow_are_rejected(self):
        for invalid in (
            WORKFLOW + "  escaped:\n    runs-on: ubuntu-latest\n    steps: []\n",
            "jobs:\n  call:\n    uses: owner/repo/.github/workflows/build.yml@main\n",
        ):
            with self.subTest(workflow=invalid), self.assertRaises(ValueError):
                budget.check_workflow(invalid)
        self.assertEqual(
            budget.check_workflow(
                f"jobs:\n  call:\n    uses: {budget.CHANGES_WORKFLOW}\n"
            ),
            set(),
        )

    def test_matrix_entries_need_fixed_slots(self):
        matrix = WORKFLOW.replace(
            "group: ci-worker-01", "group: ${{ matrix.ci_group }}"
        ).replace(
            "    steps:\n",
            "    strategy:\n      matrix:\n        include:\n"
            "          - arch: x64\n            ci_group: ci-worker-01\n"
            "          - arch: arm64\n            ci_group: ci-worker-02\n    steps:\n",
        )
        self.assertEqual(
            budget.check_workflow(matrix), {"ci-worker-01", "ci-worker-02"}
        )
        for invalid in (
            matrix.replace("            ci_group: ci-worker-02\n", ""),
            matrix.replace("ci-worker-02", "${{ github.ref }}"),
            matrix.replace(
                "  build:\n",
                "  build:\n    needs: changes\n    if: needs.changes.outputs.native_needed == 'true'\n",
            ),
            matrix.replace(
                "        include:\n", "        arch: [x64, arm64]\n        include:\n"
            ),
        ):
            with self.subTest(matrix=invalid), self.assertRaises(ValueError):
                budget.check_workflow(invalid)

    def test_flow_alias_and_duplicate_mappings_are_rejected(self):
        for invalid in (
            WORKFLOW.replace("    concurrency:\n", "    concurrency: &slot\n"),
            WORKFLOW.replace("  build:\n", "  build: &build\n"),
            WORKFLOW.replace("    steps:\n", "    <<: *unbudgeted\n    steps:\n"),
            WORKFLOW.replace(
                "      queue: max\n", "      queue: max\n      queue: single\n"
            ),
            WORKFLOW + WORKFLOW[WORKFLOW.index("  build:") :],
        ):
            with self.subTest(workflow=invalid), self.assertRaises(ValueError):
                budget.check_workflow(invalid)


class ClassifierDependenciesTests(unittest.TestCase):
    def test_dispatch_does_not_cancel_push_validation(self):
        for path in (ROOT / ".github/workflows").glob("*.y*ml"):
            source = path.read_text()
            if "  workflow_dispatch:" not in source or "  push:" not in source:
                continue
            with self.subTest(workflow=path.name):
                concurrency = budget.block(source, "concurrency", 0)
                self.assertIn("${{ github.event_name }}", concurrency)

    def test_publishing_survives_skipped_classifier_but_requires_successful_builds(self):
        cases = {
            "linux-ci.yaml": {
                "update-nightly-tag": ("build-linux",),
                "update-aur-git": ("build-linux", "validate-release-tag"),
                "update-aur-release": ("build-linux", "validate-release-tag"),
            },
            "android-ci.yaml": {
                "aab-status": ("build-apk",),
                "publish-apk": ("build-apk", "qt-ui-tests"),
            },
        }
        # These predicates need a status function to override GitHub's implicit
        # success() across the skipped PR-only classifier. Removing that alone
        # must fail even when all explicit result comparisons remain present.
        for workflow, jobs in cases.items():
            source = (ROOT / ".github/workflows" / workflow).read_text()
            for name, dependencies in jobs.items():
                job = re.search(rf"(?ms)^  {name}:\n(.*?)(?=^  [\w-]+:|\Z)", source)[1]
                predicate = re.search(r"(?m)^    if: (.*(?:\n {6,}.*)*)", job)[1]
                with self.subTest(workflow=workflow, job=name):
                    self.assertIn("!cancelled()", predicate)
                    self.assertNotIn("always()", predicate)
                    for dependency in dependencies:
                        self.assertIn(f"needs.{dependency}.result == 'success'", predicate)
                    if name == "publish-apk":
                        self.assertIn("needs.build-apk.outputs.signed == 'true'", predicate)
                    if name == "update-aur-release":
                        self.assertIn("startsWith(github.ref, 'refs/tags/v')", predicate)

    @unittest.skipUnless(
        os.name == "posix" and shutil.which("bash"), "requires a POSIX Bash runner"
    )
    def test_runner_without_ripgrep_installs_it_and_install_errors_fail(self):
        workflow = (ROOT / ".github/workflows/ci-changes.yml").read_text()
        setup = re.search(
            r"      - name: Install classifier dependencies\n        run: \|\n"
            r"((?:          .+\n)+)",
            workflow,
        )
        self.assertIsNotNone(setup)
        command = [shutil.which("bash"), "-e", "-c", textwrap.dedent(setup[1])]
        with tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            log = directory / "apt.log"
            env = {
                **os.environ,
                "PATH": tmp,
                "CI_TEST_BIN": tmp,
                "CI_TEST_APT_LOG": str(log),
                "CI_TEST_CHMOD": shutil.which("chmod"),
                "CI_TEST_APT_FAIL": "0",
            }
            commands = {
                "sudo": '#!/bin/sh\nexec "$@"\n',
                "apt-get": """#!/bin/sh
[ "$CI_TEST_APT_FAIL" = 0 ] || exit 42
printf '%s\\n' "$*" >> "$CI_TEST_APT_LOG"
if [ "$1" = install ]; then
  printf '#!/bin/sh\\nexit 0\\n' > "$CI_TEST_BIN/rg"
  "$CI_TEST_CHMOD" +x "$CI_TEST_BIN/rg"
fi
""",
            }
            for name, content in commands.items():
                path = directory / name
                path.write_text(content)
                path.chmod(0o755)
            # PATH contains no rg, matching the hosted image that failed. A
            # second invocation must reuse the tool instead of installing again.
            subprocess.run(command, env=env, check=True, capture_output=True)
            self.assertTrue(os.access(directory / "rg", os.X_OK))
            subprocess.run(command, env=env, check=True, capture_output=True)
            self.assertEqual(
                log.read_text().splitlines(),
                ["update", "install -y --no-install-recommends ripgrep"],
            )
            (directory / "rg").unlink()
            result = subprocess.run(
                command,
                env={**env, "CI_TEST_APT_FAIL": "1"},
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 42)
            self.assertFalse((directory / "rg").exists())


class FakeAPI:
    def __init__(self):
        self.pr = copy.deepcopy(PR)
        self.open_prs = []
        self.runs = [copy.deepcopy(RUN)]
        self.calls = []
        self.cancel_error = False
        self.completed = False
        self.reopen_after_listing = False

    def __call__(self, path, *, paginate=False, method="GET"):
        self.calls.append((path, method, paginate))
        if path.endswith("/pulls/12"):
            return copy.deepcopy(self.pr)
        if "/pulls?" in path:
            assert paginate
            return [[], self.open_prs]
        if "/actions/runs?" in path:
            assert paginate
            if self.reopen_after_listing:
                self.pr["state"] = "open"
            return [{"workflow_runs": []}, {"workflow_runs": copy.deepcopy(self.runs)}]
        if path.endswith("/cancel"):
            assert method == "POST"
            if self.cancel_error:
                raise subprocess.CalledProcessError(1, ["gh", "api"])
            return None
        if path.endswith("/actions/runs/34"):
            return {"status": "completed" if self.completed else "in_progress"}
        raise AssertionError(f"Unexpected request {path}")

    def cancellations(self):
        return [path for path, method, _ in self.calls if method == "POST"]


class CleanupTests(unittest.TestCase):
    def test_empty_association_after_merge_and_fork_identity(self):
        self.assertTrue(cleanup.belongs_to_pr(RUN, PR, 1))
        for field, value in (
            ("event", "push"),
            ("status", "completed"),
            ("repository", {"id": 99}),
            ("head_repository", {"id": 99}),
            ("head_repository", None),
            ("head_branch", "another-branch"),
            ("created_at", "2026-10-01T09:00:00Z"),
            ("created_at", "2026-10-01T11:01:00Z"),
            ("pull_requests", [{"number": 13}]),
            ("pull_requests", [{"number": 12}, {"number": 13}]),
        ):
            with self.subTest(field=field, value=value):
                self.assertFalse(cleanup.belongs_to_pr({**RUN, field: value}, PR, 1))

    def test_matching_explicit_association(self):
        self.assertTrue(
            cleanup.belongs_to_pr({**RUN, "pull_requests": [{"number": 12}]}, PR, 1)
        )

    def test_deleted_fork_does_not_match_another_fork(self):
        pr = copy.deepcopy(PR)
        pr["head"]["repo"] = None
        self.assertFalse(cleanup.belongs_to_pr(RUN, pr, 1))

    def test_all_pages_are_snapshotted_and_duplicate_runs_cancel_once(self):
        api = FakeAPI()
        api.runs.append({**RUN, "id": 35, "status": "pending"})
        self.assertEqual(cleanup.cancel_closed_pr("owner/repo", 12, api), [34, 35])
        self.assertEqual(len(api.cancellations()), 2)
        listing = [
            i for i, (path, _, _) in enumerate(api.calls) if "/actions/runs?" in path
        ]
        posting = [i for i, (_, method, _) in enumerate(api.calls) if method == "POST"]
        self.assertLess(max(listing), min(posting))

    def test_open_pr_and_reopened_pr_are_preserved(self):
        for reopen_later in (False, True):
            with self.subTest(reopen_later=reopen_later):
                api = FakeAPI()
                if reopen_later:
                    api.reopen_after_listing = True
                else:
                    api.pr["state"] = "open"
                self.assertEqual(cleanup.cancel_closed_pr("owner/repo", 12, api), [])
                self.assertEqual(api.cancellations(), [])

    def test_other_open_pr_on_same_source_branch_is_preserved(self):
        api = FakeAPI()
        api.open_prs = [{**PR, "number": 13, "state": "open"}]
        self.assertEqual(cleanup.cancel_closed_pr("owner/repo", 12, api), [])
        self.assertEqual(api.cancellations(), [])

    def test_other_fork_with_same_branch_does_not_block_cleanup(self):
        api = FakeAPI()
        other = copy.deepcopy(PR)
        other["head"]["repo"]["id"] = 99
        api.open_prs = [other]
        self.assertEqual(cleanup.cancel_closed_pr("owner/repo", 12, api), [34])

    def test_completion_race_is_harmless_but_api_failure_is_not_hidden(self):
        api = FakeAPI()
        api.cancel_error = True
        api.completed = True
        self.assertEqual(cleanup.cancel_closed_pr("owner/repo", 12, api), [])
        api.completed = False
        with self.assertRaises(subprocess.CalledProcessError):
            cleanup.cancel_closed_pr("owner/repo", 12, api)


if __name__ == "__main__":
    unittest.main()
