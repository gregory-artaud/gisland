#!/usr/bin/env bash
set -euo pipefail

sway=$1
xvfb=$2
raylib_reference=$3
portable_reference=$4
prototype=$5
runtime_dir=$(mktemp -d)
chmod 700 "${runtime_dir}"
log_file="${runtime_dir}/sway.log"
config_file="${runtime_dir}/sway.conf"
display_file="${runtime_dir}/xvfb-display"
expected_rgba="${runtime_dir}/expected.rgba"
portable_rgba="${runtime_dir}/portable.rgba"
cat >"${config_file}" <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x720
EOF

cleanup() {
  if [[ -n "${sway_pid:-}" ]]; then
    kill "${sway_pid}" 2>/dev/null || true
    wait "${sway_pid}" 2>/dev/null || true
  fi
  if [[ -n "${xvfb_pid:-}" ]]; then
    kill "${xvfb_pid}" 2>/dev/null || true
    wait "${xvfb_pid}" 2>/dev/null || true
  fi
  rm -rf "${runtime_dir}"
}
trap cleanup EXIT

"${xvfb}" -displayfd 3 -screen 0 640x480x24 -nolisten tcp 3>"${display_file}" \
  >/dev/null 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 50); do
  if [[ -s "${display_file}" ]]; then
    break
  fi
  sleep 0.05
done
if [[ ! -s "${display_file}" ]]; then
  exit 1
fi
IFS= read -r display_number <"${display_file}"
DISPLAY=":${display_number}" LIBGL_ALWAYS_SOFTWARE=1 "${raylib_reference}" "${expected_rgba}"
DISPLAY=":${display_number}" LIBGL_ALWAYS_SOFTWARE=1 \
  "${portable_reference}" "${portable_rgba}"
if ! cmp -s "${expected_rgba}" "${portable_rgba}"; then
  printf 'Raylib and portable X11 RGBA outputs differ\n' >&2
  exit 1
fi
kill "${xvfb_pid}" 2>/dev/null || true
wait "${xvfb_pid}" 2>/dev/null || true
unset xvfb_pid

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
    GISLAND_EXPECTED_RGBA_PATH="${expected_rgba}" LIBGL_ALWAYS_SOFTWARE=1 \
      "${prototype}" --automated --width 480 --height 360
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
