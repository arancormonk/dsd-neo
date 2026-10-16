#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cancel obsolete pull_request runs using trusted repository metadata only."""

import argparse
import datetime
import json
import subprocess
import sys
import urllib.parse

ACTIVE_STATUSES = ("queued", "in_progress", "pending", "waiting", "requested")


def timestamp(value):
    # Python before 3.11 accepts ISO offsets, but not GitHub's trailing Z.
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    return datetime.datetime.fromisoformat(value)


def belongs_to_pr(run, pr, repository_id):
    """Never use a branch name alone: different forks can have the same name.

    GitHub can empty pull_requests after merge, including for same-repository PRs.
    In that case require the source repository, branch and PR lifetime to match.
    The caller also excludes branches with an open PR before using this fallback.
    """
    if run.get("event") != "pull_request" or run.get("status") not in ACTIVE_STATUSES:
        return False
    if run.get("repository", {}).get("id") != repository_id:
        return False
    associations = run.get("pull_requests", [])
    if associations:
        return all(p.get("number") == pr["number"] for p in associations)
    head_repo = (pr.get("head", {}).get("repo") or {}).get("id")
    if not head_repo or (run.get("head_repository") or {}).get("id") != head_repo:
        return False
    if run.get("head_branch") != pr["head"]["ref"]:
        return False
    created = run.get("created_at")
    if not created or not pr.get("closed_at"):
        return False
    return (
        timestamp(pr["created_at"]) <= timestamp(created) <= timestamp(pr["closed_at"])
    )


def api(path, *, paginate=False, method="GET"):
    args = ["gh", "api", "--method", method, path]
    if paginate:
        args.extend(["--paginate", "--slurp"])
    result = subprocess.run(args, check=True, text=True, capture_output=True)
    return json.loads(result.stdout) if result.stdout.strip() else None


def cancel_closed_pr(repository, number, request=api):
    prefix = f"repos/{repository}"
    pr = request(f"{prefix}/pulls/{number}")
    if pr["state"] != "closed":
        print(f"PR #{number} is open; nothing to cancel")
        return []
    repository_id = pr["base"]["repo"]["id"]
    source_id = (pr["head"].get("repo") or {}).get("id")
    # Snapshot all pages before cancellation changes the result set. Also avoid
    # canceling work for a second PR, or a reopened PR, using this source branch.
    open_prs = request(f"{prefix}/pulls?state=open&per_page=100", paginate=True)
    for page in open_prs:
        for other in page:
            if (other["head"].get("repo") or {}).get("id") == source_id and other[
                "head"
            ]["ref"] == pr["head"]["ref"]:
                print("Source branch has an open PR; retaining its runs")
                return []
    candidates = {}
    for status in ACTIVE_STATUSES:
        query = urllib.parse.urlencode(
            {
                "event": "pull_request",
                "status": status,
                "branch": pr["head"]["ref"],
                "per_page": 100,
            }
        )
        for page in request(f"{prefix}/actions/runs?{query}", paginate=True):
            for run in page["workflow_runs"]:
                if belongs_to_pr(run, pr, repository_id):
                    candidates[run["id"]] = run
    canceled = []
    for run_id in candidates:
        # A close event can wait for a slot while somebody reopens the PR.
        current = request(f"{prefix}/pulls/{number}")
        if current["state"] != "closed" or current["closed_at"] != pr["closed_at"]:
            print("PR state changed during cleanup; stopping")
            break
        try:
            request(f"{prefix}/actions/runs/{run_id}/cancel", method="POST")
        except subprocess.CalledProcessError:
            # Finishing between the listing and cancel request is harmless;
            # authentication/server failures while still active must stay red.
            if request(f"{prefix}/actions/runs/{run_id}")["status"] != "completed":
                raise
        else:
            canceled.append(run_id)
            print(f"Canceled obsolete PR #{number} run {run_id}")
    return canceled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--pr", required=True, type=int)
    args = parser.parse_args()
    if args.pr <= 0 or len(args.repository.split("/")) != 2:
        parser.error("a repository owner/name and positive PR number are required")
    cancel_closed_pr(args.repository, args.pr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
