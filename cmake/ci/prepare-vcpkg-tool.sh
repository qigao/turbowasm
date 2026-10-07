#!/usr/bin/env bash
set -euo pipefail

# Bootstrap the shared contract's tool before its setup action verifies it.
# Hosted runner images roll independently and cannot own this version choice.
contract_root="${1:?shared vcpkg cache contract checkout is required}"
scripts_revision="$(tr -d '\r\n' < "$contract_root/vcpkg-scripts-revision.txt")"
expected_version="$(tr -d '\r\n' < "$contract_root/vcpkg-tool-version.txt")"
[[ "$scripts_revision" =~ ^[0-9a-f]{40}$ ]]
test -n "$expected_version"

tool_root="$(mktemp -d "${RUNNER_TEMP:?}/turbowasm-vcpkg-tool.XXXXXX")"
git -C "$tool_root" init --quiet
git -C "$tool_root" remote add origin https://github.com/microsoft/vcpkg.git
git -C "$tool_root" fetch --depth=1 --no-tags origin "$scripts_revision"
git -C "$tool_root" checkout --detach FETCH_HEAD
# The upstream bootstrap verifies the executable hash from the pinned scripts.
sh "$tool_root/bootstrap-vcpkg.sh" -disableMetrics
actual_version="$("$tool_root/vcpkg" version | awk '/^vcpkg package management program version / {print $NF; exit}')"
if [ "$actual_version" != "$expected_version" ]; then
  echo "bootstrapped vcpkg version mismatch: $actual_version != $expected_version" >&2
  exit 1
fi
echo "VCPKG_ROOT=$tool_root" >> "${GITHUB_ENV:?}"
