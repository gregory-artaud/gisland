#!/usr/bin/env bash

set -euo pipefail

if [[ -z ${HOME:-} ]]; then
  printf 'install-local: HOME is not set\n' >&2
  exit 1
fi

for command_name in cmake cmp ninja systemctl; do
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
service_was_enabled=unknown
service_state_captured=false

recover_service() {
  local status=$?
  trap - EXIT
  if ((status != 0)) && [[ $service_state_captured == true ]]; then
    printf 'install-local: attempting to restore gisland.service\n' >&2
    if [[ $service_was_enabled == true ]]; then
      systemctl --user enable gisland.service || true
    elif [[ $service_was_enabled == false ]]; then
      systemctl --user disable gisland.service || true
    fi
    if [[ $service_was_active == true ]]; then
      systemctl --user start gisland.service || true
    else
      systemctl --user stop gisland.service || true
    fi
  fi
  exit "$status"
}
trap recover_service EXIT

cd -- "$source_dir"

cmake --preset release -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build --preset release

if systemctl --user is-active --quiet gisland.service; then
  service_was_active=true
fi
service_enabled_state=$(systemctl --user is-enabled gisland.service 2>/dev/null || true)
if [[ $service_enabled_state == enabled ]]; then
  service_was_enabled=true
elif [[ $service_enabled_state == disabled ]]; then
  service_was_enabled=false
fi
service_state_captured=true

if [[ $service_was_active == true ]]; then
  systemctl --user stop gisland.service
fi

cmake --install build/release

if ! cmp -s -- "$source_dir/build/release/gisland-lua-host" \
  "$install_prefix/bin/gisland-lua-host"; then
  printf 'install-local: installed Lua host is not current: %s\n' \
    "$install_prefix/bin/gisland-lua-host" >&2
  exit 1
fi

verify_current_files() {
  local package_name=$1
  local -n sources=$2
  local -n destinations=$3
  local index
  for index in "${!sources[@]}"; do
    if ! cmp -s -- "${sources[$index]}" "${destinations[$index]}"; then
      printf 'install-local: installed %s file is not current: %s\n' \
        "$package_name" "${destinations[$index]}" >&2
      exit 1
    fi
  done
}

replacement_clock_files=(
  "$install_prefix/bin/gisland-lua-host"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/module.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/config.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/view.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/clock_calendar.lua"
)
for replacement_clock_file in "${replacement_clock_files[@]}"; do
  if [[ ! -f $replacement_clock_file ]]; then
    printf 'install-local: replacement clock-calendar file was not installed: %s\n' \
      "$replacement_clock_file" >&2
    exit 1
  fi
done
clock_sources=(
  "$source_dir/build/release/install/clock-calendar.module.toml"
  "$source_dir/assets/modules/clock-calendar/config.toml"
  "$source_dir/assets/modules/clock-calendar/view.toml"
  "$source_dir/assets/modules/clock-calendar/clock_calendar.lua"
)
clock_destinations=(
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/module.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/config.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/view.toml"
  "$install_prefix/share/gisland/distributed/modules/clock-calendar/clock_calendar.lua"
)
verify_current_files clock-calendar clock_sources clock_destinations

replacement_audio_files=(
  "$install_prefix/bin/gisland-lua-host"
  "$install_prefix/share/gisland/distributed/modules/audio/module.toml"
  "$install_prefix/share/gisland/distributed/modules/audio/config.toml"
  "$install_prefix/share/gisland/distributed/modules/audio/audio.lua"
  "$install_prefix/share/gisland/distributed/modules/audio/command.lua"
)
for replacement_audio_file in "${replacement_audio_files[@]}"; do
  if [[ ! -f $replacement_audio_file ]]; then
    printf 'install-local: replacement audio file was not installed: %s\n' \
      "$replacement_audio_file" >&2
    exit 1
  fi
done
audio_sources=(
  "$source_dir/build/release/install/audio.module.toml"
  "$source_dir/assets/modules/audio/config.toml"
  "$source_dir/assets/modules/audio/audio.lua"
  "$source_dir/assets/modules/audio/command.lua"
)
audio_destinations=(
  "$install_prefix/share/gisland/distributed/modules/audio/module.toml"
  "$install_prefix/share/gisland/distributed/modules/audio/config.toml"
  "$install_prefix/share/gisland/distributed/modules/audio/audio.lua"
  "$install_prefix/share/gisland/distributed/modules/audio/command.lua"
)
verify_current_files audio audio_sources audio_destinations

replacement_battery_files=(
  "$install_prefix/bin/gisland-lua-host"
  "$install_prefix/share/gisland/distributed/modules/battery/module.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/config.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/view.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/battery.lua"
)
for replacement_battery_file in "${replacement_battery_files[@]}"; do
  if [[ ! -f $replacement_battery_file ]]; then
    printf 'install-local: replacement battery file was not installed: %s\n' \
      "$replacement_battery_file" >&2
    exit 1
  fi
done

battery_sources=(
  "$source_dir/build/release/install/battery.module.toml"
  "$source_dir/assets/modules/battery/config.toml"
  "$source_dir/assets/modules/battery/view.toml"
  "$source_dir/assets/modules/battery/battery.lua"
)
battery_destinations=(
  "$install_prefix/share/gisland/distributed/modules/battery/module.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/config.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/view.toml"
  "$install_prefix/share/gisland/distributed/modules/battery/battery.lua"
)
verify_current_files battery battery_sources battery_destinations

replacement_notification_files=(
  "$install_prefix/bin/gisland-lua-host"
  "$install_prefix/share/gisland/distributed/modules/notifications/module.toml"
  "$install_prefix/share/gisland/distributed/modules/notifications/notifications.lua"
)
for replacement_notification_file in "${replacement_notification_files[@]}"; do
  if [[ ! -f $replacement_notification_file ]]; then
    printf 'install-local: replacement notification file was not installed: %s\n' \
      "$replacement_notification_file" >&2
    exit 1
  fi
done
notification_sources=(
  "$source_dir/build/release/install/notifications.module.toml"
  "$source_dir/assets/modules/notifications/notifications.lua"
)
notification_destinations=(
  "$install_prefix/share/gisland/distributed/modules/notifications/module.toml"
  "$install_prefix/share/gisland/distributed/modules/notifications/notifications.lua"
)
verify_current_files notification notification_sources notification_destinations

rm -f -- "$install_prefix/bin/gisland-clock-calendar"
rm -f -- "$install_prefix/share/gisland/distributed/modules/clock-calendar/calendar.lua"
rm -f -- "$install_prefix/bin/gisland-audio"
rm -f -- "$install_prefix/bin/gisland-audio-control"
rm -rf -- "$install_prefix/share/gisland/audio/gisland_audio"
rmdir -- "$install_prefix/share/gisland/audio" 2>/dev/null || true
rm -rf -- "$install_prefix/share/gisland/distributed/modules/audio-lua"
rm -f -- "$install_prefix/bin/gisland-battery"
rm -rf -- "$install_prefix/share/gisland/battery/gisland_battery"
rmdir -- "$install_prefix/share/gisland/battery" 2>/dev/null || true
rm -f -- "$install_prefix/bin/gisland-notifications"
rm -f -- "$install_prefix/bin/gisland-notification-history"
rm -rf -- "$install_prefix/share/gisland/notifications/gisland_notifications"
rmdir -- "$install_prefix/share/gisland/notifications" 2>/dev/null || true

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

systemctl --user start gisland.service

for ((attempt = 0; attempt < 20; ++attempt)); do
  if "$installed_control" status >/dev/null 2>&1; then
    systemctl --user enable gisland.service
    service_state_captured=false
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
