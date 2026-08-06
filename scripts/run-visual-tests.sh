#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build/graphical}"
xvfb="${XVFB_EXECUTABLE:-Xvfb}"

if ! command -v "${xvfb}" >/dev/null 2>&1; then
  printf 'Xvfb executable not found: %s\n' "${xvfb}" >&2
  exit 1
fi
if ! command -v dbus-run-session >/dev/null 2>&1; then
  printf 'dbus-run-session executable not found\n' >&2
  exit 1
fi

display_file="$(mktemp)"
trap 'rm -f "${display_file}"' EXIT
"${xvfb}" -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"${display_file}" \
  >/dev/null 2>&1 &
xvfb_pid=$!
trap 'kill "${xvfb_pid}" 2>/dev/null || true; wait "${xvfb_pid}" 2>/dev/null || true; rm -f "${display_file}"' EXIT

for _ in $(seq 1 50); do
  if [[ -s "${display_file}" ]]; then
    break
  fi
  sleep 0.1
done

if [[ ! -s "${display_file}" ]]; then
  printf 'Xvfb did not allocate a display\n' >&2
  exit 1
fi
IFS= read -r display_number <"${display_file}"
export DISPLAY=":${display_number}"

if ! xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then
  printf 'Xvfb did not become ready on %s\n' "${DISPLAY}" >&2
  exit 1
fi

export LIBGL_ALWAYS_SOFTWARE=1
export LC_ALL=C
export TZ=UTC
dbus-run-session -- ctest --test-dir "${build_dir}" --output-on-failure \
  -R 'input shape does not clip|^application_x11::|^x11_window::|^raylib_renderer::|^visual_regression::'
