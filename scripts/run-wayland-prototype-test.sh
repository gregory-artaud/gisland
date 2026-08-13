#!/usr/bin/env bash
set -euo pipefail

sway=$1
prototype=$2
runtime_dir=$(mktemp -d)
chmod 700 "${runtime_dir}"
log_file="${runtime_dir}/sway.log"
config_file="${runtime_dir}/sway.conf"
cat >"${config_file}" <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x720
EOF

cleanup() {
  if [[ -n "${sway_pid:-}" ]]; then
    kill "${sway_pid}" 2>/dev/null || true
    wait "${sway_pid}" 2>/dev/null || true
  fi
  rm -rf "${runtime_dir}"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="${runtime_dir}"
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
unset WAYLAND_DISPLAY
"${sway}" --config "${config_file}" --unsupported-gpu >"${log_file}" 2>&1 &
sway_pid=$!

for _ in $(seq 1 100); do
  wayland_socket=$(compgen -G "${runtime_dir}/wayland-*" | while read -r candidate; do
    [[ -S "${candidate}" ]] && basename "${candidate}" && break
  done || true)
  if [[ -n "${wayland_socket}" ]]; then
    export WAYLAND_DISPLAY="${wayland_socket}"
    LIBGL_ALWAYS_SOFTWARE=1 "${prototype}" --automated
    exit 0
  fi
  if ! kill -0 "${sway_pid}" 2>/dev/null; then
    cat "${log_file}" >&2
    exit 1
  fi
  sleep 0.05
done

cat "${log_file}" >&2
exit 1
