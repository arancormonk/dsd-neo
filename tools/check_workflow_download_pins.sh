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
  INSTALL_OPENSUSE_LEAP_156_IMAGE \
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

for var in CMAKE_LINUX_X86_64_SHA256 CMAKE_LINUX_AARCH64_SHA256; do
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

exit "$failed"
