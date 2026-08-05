#!/usr/bin/env bash

set -euo pipefail

if [[ -z ${HOME:-} ]]; then
  printf 'install-local: HOME is not set\n' >&2
  exit 1
fi

for command_name in cmake ninja systemctl; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'install-local: required command not found: %s\n' "$command_name" >&2
    exit 1
  fi
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$script_dir/.." && pwd)
install_prefix="$HOME/.local"
installed_control="$install_prefix/bin/gislandctl"

service_was_active=false
service_stopped=false

recover_service() {
  local status=$?
  trap - EXIT
  if ((status != 0)) && [[ $service_was_active == true && $service_stopped == true ]]; then
    printf 'install-local: attempting to restore gisland.service\n' >&2
    systemctl --user start gisland.service || true
  fi
  exit "$status"
}
trap recover_service EXIT

cd -- "$source_dir"

cmake --preset release -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build --preset release

if systemctl --user is-active --quiet gisland.service; then
  service_was_active=true
  systemctl --user stop gisland.service
  service_stopped=true
fi

cmake --install build/release
systemctl --user daemon-reload

environment_names=()
if [[ -n ${DISPLAY:-} ]]; then
  environment_names+=(DISPLAY)
fi
if [[ -n ${XAUTHORITY:-} ]]; then
  environment_names+=(XAUTHORITY)
fi
if ((${#environment_names[@]} > 0)); then
  systemctl --user import-environment "${environment_names[@]}"
fi

systemctl --user enable --now gisland.service

for ((attempt = 0; attempt < 20; ++attempt)); do
  if "$installed_control" status >/dev/null 2>&1; then
    service_stopped=false
    printf 'gisland installed successfully under %s\n' "$install_prefix"
    exit 0
  fi
  sleep 0.25
done

printf 'install-local: gisland.service did not become healthy\n' >&2
if command -v journalctl >/dev/null 2>&1; then
  journalctl --user -u gisland.service --no-pager -n 20 >&2 || true
fi
exit 1
