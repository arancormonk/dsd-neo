# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
# shellcheck shell=bash
#
# Default worker count for the per-translation-unit analyzers (clang-tidy, IWYU,
# GCC -fanalyzer).
#
# Each of them used to default to `nproc` alone. That is the wrong resource to
# size by: a clang-tidy or IWYU worker holds a few hundred MB on an ordinary
# translation unit and approaches a gigabyte on the Qt ones, so a host with many
# cores and little memory per core starts more workers than it can feed. What
# follows is not a hypothetical: an analyzer worker killed by the OOM killer
# exits non-zero having emitted no diagnostics, which the completeness gates now
# correctly refuse to read as a clean translation unit - so oversubscription
# turns into a failed run rather than a slow one.
#
# The floor is 1, and a host that does not publish its available memory (no
# /proc/meminfo, i.e. not Linux) keeps the CPU count, which is the behaviour
# every caller had before.

# dsd_detect_cpus: usable CPU count, 4 if nothing will say.
dsd_detect_cpus() {
  local cpus=""
  cpus=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)
  if [[ ! "$cpus" =~ ^[1-9][0-9]*$ ]]; then
    cpus=4
  fi
  printf '%s\n' "$cpus"
}

# dsd_cgroup_available_mb: headroom left in this process's memory cgroup, in MB;
# empty when there is no limit to speak of. /proc/meminfo reports the host's
# memory even inside a container, so without this the analyzers size themselves
# against a machine they cannot touch - which is the case that matters, since
# the CI static-analysis jobs run under `docker run`.
dsd_cgroup_available_mb() {
  local limit="" usage=""
  # cgroup v2 first: unified hierarchy, and "max" means no limit.
  if [[ -r /sys/fs/cgroup/memory.max && -r /sys/fs/cgroup/memory.current ]]; then
    limit=$(cat /sys/fs/cgroup/memory.max 2> /dev/null || true)
    usage=$(cat /sys/fs/cgroup/memory.current 2> /dev/null || true)
  elif [[ -r /sys/fs/cgroup/memory/memory.limit_in_bytes && -r /sys/fs/cgroup/memory/memory.usage_in_bytes ]]; then
    limit=$(cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2> /dev/null || true)
    usage=$(cat /sys/fs/cgroup/memory/memory.usage_in_bytes 2> /dev/null || true)
  fi
  if [[ ! "$limit" =~ ^[0-9]+$ || ! "$usage" =~ ^[0-9]+$ ]]; then
    return 0
  fi
  # cgroup v1 spells "unlimited" as a number near 2^63, which is not a limit any
  # host can honour; anything past a petabyte is that sentinel, not a budget.
  if [[ ${#limit} -gt 16 ]]; then
    return 0
  fi
  if [[ $usage -ge $limit ]]; then
    printf '%s\n' 0
    return 0
  fi
  printf '%s\n' "$(((limit - usage) / 1024 / 1024))"
}

# dsd_available_mb: memory a new process can expect to get without swapping, in
# MB; empty when nothing will say. MemAvailable rather than MemFree because page
# cache is reclaimable and MemFree reads near zero on a warm machine. Whichever
# of the host figure and the cgroup headroom is smaller wins: both are ceilings,
# and the run is held to the lower one.
dsd_available_mb() {
  local kb="" host="" cgroup=""
  if [[ -r /proc/meminfo ]]; then
    kb=$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo 2> /dev/null || true)
  fi
  if [[ "$kb" =~ ^[0-9]+$ ]]; then
    host=$((kb / 1024))
  fi
  cgroup=$(dsd_cgroup_available_mb)
  if [[ -z "$host" ]]; then
    printf '%s\n' "$cgroup"
    return 0
  fi
  if [[ -n "$cgroup" && $cgroup -lt $host ]]; then
    printf '%s\n' "$cgroup"
    return 0
  fi
  printf '%s\n' "$host"
}

# dsd_default_jobs MB_PER_WORKER: min(CPU count, available memory / MB_PER_WORKER),
# floored at 1. Callers pass what one worker of theirs actually costs.
dsd_default_jobs() {
  local mb_per_worker="$1"
  local cpus="" avail="" by_mem=""
  cpus=$(dsd_detect_cpus)
  avail=$(dsd_available_mb)
  if [[ -z "$avail" || ! "$mb_per_worker" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' "$cpus"
    return 0
  fi
  by_mem=$((avail / mb_per_worker))
  if [[ $by_mem -lt 1 ]]; then
    by_mem=1
  fi
  if [[ $by_mem -lt $cpus ]]; then
    printf '%s\n' "$by_mem"
    return 0
  fi
  printf '%s\n' "$cpus"
}

# dsd_report_jobs TOOL MB_PER_WORKER JOBS: say so when memory, not the core
# count, is what set the worker count - otherwise a run that is slower than the
# machine looks unexplained.
dsd_report_jobs() {
  local tool="$1" mb_per_worker="$2" jobs="$3"
  local cpus=""
  cpus=$(dsd_detect_cpus)
  if [[ $jobs -lt $cpus ]]; then
    echo "${tool}: using ${jobs} worker(s) rather than ${cpus}: about ${mb_per_worker} MB each, $(dsd_available_mb) MB available." >&2
  fi
}
