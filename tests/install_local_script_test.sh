#!/usr/bin/env bash

set -euo pipefail

project_source_dir=${1:?usage: install_local_script_test.sh SOURCE_DIR}
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT
source_dir="$test_root/source"
mkdir -p "$source_dir/scripts" "$source_dir/assets/modules/battery" \
  "$source_dir/assets/modules/clock-calendar" \
  "$source_dir/assets/modules/audio" \
  "$source_dir/assets/modules/notifications" \
  "$source_dir/build/release/install"
cp "$project_source_dir/scripts/install-local.sh" "$source_dir/scripts/install-local.sh"
cp "$project_source_dir/assets/modules/clock-calendar/config.toml" \
  "$project_source_dir/assets/modules/clock-calendar/view.toml" \
  "$project_source_dir/assets/modules/clock-calendar/clock_calendar.lua" \
  "$source_dir/assets/modules/clock-calendar/"
printf 'current-clock-manifest\n' \
  >"$source_dir/build/release/install/clock-calendar.module.toml"
cp "$project_source_dir/assets/modules/audio/config.toml" \
  "$project_source_dir/assets/modules/audio/audio.lua" \
  "$project_source_dir/assets/modules/audio/command.lua" \
  "$source_dir/assets/modules/audio/"
printf 'current-audio-manifest\n' >"$source_dir/build/release/install/audio.module.toml"
cp "$project_source_dir/assets/modules/battery/config.toml" \
  "$project_source_dir/assets/modules/battery/view.toml" \
  "$project_source_dir/assets/modules/battery/battery.lua" \
  "$source_dir/assets/modules/battery/"
printf 'current-battery-manifest\n' >"$source_dir/build/release/install/battery.module.toml"
cp "$project_source_dir/assets/modules/notifications/notifications.lua" \
  "$source_dir/assets/modules/notifications/notifications.lua"
printf 'current-notifications-manifest\n' \
  >"$source_dir/build/release/install/notifications.module.toml"
printf 'current-lua-host\n' >"$source_dir/build/release/gisland-lua-host"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

assert_contains() {
  local file=$1
  local expected=$2
  grep -Fq -- "$expected" "$file" || fail "missing '$expected' in $file"
}

assert_not_contains() {
  local file=$1
  local unexpected=$2
  if grep -Fq -- "$unexpected" "$file"; then
    fail "unexpected '$unexpected' in $file"
  fi
}

assert_exists() {
  [[ -e $1 ]] || fail "missing expected path: $1"
}

assert_not_exists() {
  [[ ! -e $1 ]] || fail "unexpected path: $1"
}

assert_service_state() {
  local case_dir=$1
  local active=$2
  local enabled=$3
  [[ $(<"$case_dir/service-active") == "$active" ]] ||
    fail "unexpected active state for $case_dir"
  [[ $(<"$case_dir/service-enabled") == "$enabled" ]] ||
    fail "unexpected enabled state for $case_dir"
}

line_number() {
  local file=$1
  local expected=$2
  local current=0
  local line
  while IFS= read -r line; do
    ((current += 1))
    if [[ $line == *"$expected"* ]]; then
      printf '%s\n' "$current"
      return 0
    fi
  done <"$file"
  return 1
}

make_case() {
  local name=$1
  local case_dir="$test_root/$name"
  mkdir -p "$case_dir/bin" "$case_dir/home/.config/gisland" \
    "$case_dir/xdg-config/gisland" "$case_dir/xdg-data/gisland" \
    "$case_dir/home/.local/bin" \
    "$case_dir/home/.local/share/gisland/audio/gisland_audio" \
    "$case_dir/home/.local/share/gisland/battery/gisland_battery" \
    "$case_dir/home/.local/share/gisland/notifications/gisland_notifications" \
    "$case_dir/home/.local/share/gisland/distributed/modules/notifications" \
    "$case_dir/home/.local/state/gisland" \
    "$case_dir/home/.local/share/gisland/distributed/modules/battery" \
    "$case_dir/home/.local/share/gisland/distributed/modules/clock-calendar" \
    "$case_dir/home/.local/share/gisland/distributed/modules/audio-lua" \
    "$case_dir/home/.local/share/gisland/modules"
  printf 'keep-config\n' >"$case_dir/home/.config/gisland/config.toml"
  printf 'keep-xdg-config\n' >"$case_dir/xdg-config/gisland/sentinel"
  printf 'keep-xdg-data\n' >"$case_dir/xdg-data/gisland/sentinel"
  printf 'keep-module\n' >"$case_dir/home/.local/share/gisland/modules/sentinel"
  printf 'legacy-audio\n' >"$case_dir/home/.local/bin/gisland-audio"
  printf 'legacy-control\n' >"$case_dir/home/.local/bin/gisland-audio-control"
  printf 'legacy-clock\n' >"$case_dir/home/.local/bin/gisland-clock-calendar"
  printf 'legacy-battery\n' >"$case_dir/home/.local/bin/gisland-battery"
  printf 'legacy-notifications\n' >"$case_dir/home/.local/bin/gisland-notifications"
  printf 'legacy-history\n' >"$case_dir/home/.local/bin/gisland-notification-history"
  printf 'unfingerprinted-clock-helper\n' \
    >"$case_dir/home/.local/share/gisland/distributed/modules/clock-calendar/calendar.lua"
  printf 'legacy-package\n' \
    >"$case_dir/home/.local/share/gisland/audio/gisland_audio/application.py"
  mkdir -p "$case_dir/home/.local/share/gisland/audio/gisland_audio/__pycache__"
  printf 'legacy-cache\n' \
    >"$case_dir/home/.local/share/gisland/audio/gisland_audio/__pycache__/application.pyc"
  printf 'keep-audio-sibling\n' \
    >"$case_dir/home/.local/share/gisland/audio/sentinel"
  printf 'legacy-battery-package\n' \
    >"$case_dir/home/.local/share/gisland/battery/gisland_battery/application.py"
  printf 'keep-battery-sibling\n' \
    >"$case_dir/home/.local/share/gisland/battery/sentinel"
  printf 'legacy-notification-package\n' \
    >"$case_dir/home/.local/share/gisland/notifications/gisland_notifications/application.py"
  printf 'keep-notification-sibling\n' \
    >"$case_dir/home/.local/share/gisland/notifications/sentinel"
  printf 'preserved-history\n' \
    >"$case_dir/home/.local/state/gisland/notifications-history.json"
  for package_file in module.toml config.toml view.toml clock_calendar.lua; do
    printf 'stale-clock\n' \
      >"$case_dir/home/.local/share/gisland/distributed/modules/clock-calendar/$package_file"
  done
  mkdir -p "$case_dir/home/.local/share/gisland/distributed/modules/audio"
  for package_file in module.toml config.toml audio.lua command.lua; do
    printf 'stale-audio\n' \
      >"$case_dir/home/.local/share/gisland/distributed/modules/audio/$package_file"
  done
  for package_file in module.toml config.toml view.toml battery.lua; do
    printf 'stale-battery\n' \
      >"$case_dir/home/.local/share/gisland/distributed/modules/battery/$package_file"
  done
  printf 'legacy-candidate\n' \
    >"$case_dir/home/.local/share/gisland/distributed/modules/audio-lua/module.toml"
  printf 'stale-notification-manifest\n' \
    >"$case_dir/home/.local/share/gisland/distributed/modules/notifications/module.toml"
  printf 'stale-notification-module\n' \
    >"$case_dir/home/.local/share/gisland/distributed/modules/notifications/notifications.lua"
  printf 'keep-bin\n' >"$case_dir/home/.local/bin/unrelated-tool"
  : >"$case_dir/commands.log"

  cat >"$case_dir/bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'cmake|%s\n' "$*" >>"$TEST_COMMAND_LOG"
if [[ ${FAKE_CMAKE_FAILURE:-} == build && ${1:-} == --build ]]; then
  exit 17
fi
  if [[ ${1:-} == --install ]]; then
  if [[ ${FAKE_CMAKE_FAILURE:-} == install ]]; then
    exit 18
  fi
  mkdir -p "$HOME/.local/bin"
    cp "$FAKE_BIN_DIR/gislandctl-template" "$HOME/.local/bin/gislandctl"
    chmod +x "$HOME/.local/bin/gislandctl"
    if [[ ${FAKE_CMAKE_MISSING_HOST:-0} != 1 ]]; then
      cp build/release/gisland-lua-host "$HOME/.local/bin/gisland-lua-host"
      chmod +x "$HOME/.local/bin/gisland-lua-host"
    fi
    if [[ ${FAKE_CMAKE_MISSING_CLOCK:-0} != 1 ]]; then
      mkdir -p "$HOME/.local/share/gisland/distributed/modules/clock-calendar"
      cp build/release/install/clock-calendar.module.toml \
        "$HOME/.local/share/gisland/distributed/modules/clock-calendar/module.toml"
      cp assets/modules/clock-calendar/config.toml \
        assets/modules/clock-calendar/view.toml \
        assets/modules/clock-calendar/clock_calendar.lua \
        "$HOME/.local/share/gisland/distributed/modules/clock-calendar/"
    fi
    if [[ ${FAKE_CMAKE_MISSING_AUDIO:-0} != 1 ]]; then
      mkdir -p "$HOME/.local/share/gisland/distributed/modules/audio"
      cp build/release/install/audio.module.toml \
        "$HOME/.local/share/gisland/distributed/modules/audio/module.toml"
      cp assets/modules/audio/config.toml assets/modules/audio/audio.lua \
        assets/modules/audio/command.lua \
        "$HOME/.local/share/gisland/distributed/modules/audio/"
    fi
    if [[ ${FAKE_CMAKE_MISSING_BATTERY:-0} != 1 ]]; then
      mkdir -p "$HOME/.local/share/gisland/distributed/modules/battery"
      cp build/release/install/battery.module.toml \
        "$HOME/.local/share/gisland/distributed/modules/battery/module.toml"
      cp assets/modules/battery/config.toml assets/modules/battery/view.toml \
        assets/modules/battery/battery.lua \
        "$HOME/.local/share/gisland/distributed/modules/battery/"
    fi
    if [[ ${FAKE_CMAKE_MISSING_NOTIFICATIONS:-0} != 1 ]]; then
      mkdir -p "$HOME/.local/share/gisland/distributed/modules/notifications"
      cp build/release/install/notifications.module.toml \
        "$HOME/.local/share/gisland/distributed/modules/notifications/module.toml"
      cp assets/modules/notifications/notifications.lua \
        "$HOME/.local/share/gisland/distributed/modules/notifications/notifications.lua"
    fi
fi
EOF

  cat >"$case_dir/bin/rm" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'rm|%s\n' "$*" >>"$TEST_COMMAND_LOG"
if [[ -n ${FAKE_RM_FAIL_MATCH:-} && $* == *"$FAKE_RM_FAIL_MATCH"* ]]; then
  exit 19
fi
/bin/rm "$@"
EOF

  cat >"$case_dir/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'systemctl|%s\n' "$*" >>"$TEST_COMMAND_LOG"
state_dir=${TEST_COMMAND_LOG%/*}
active_file="$state_dir/service-active"
enabled_file="$state_dir/service-enabled"
[[ -f $active_file ]] || printf '%s\n' "${FAKE_SERVICE_ACTIVE:-0}" >"$active_file"
[[ -f $enabled_file ]] || {
  if [[ ${FAKE_SERVICE_ENABLED:-0} == 1 ]]; then
    printf 'enabled\n' >"$enabled_file"
  else
    printf '%s\n' "${FAKE_SERVICE_ENABLED_STATE:-disabled}" >"$enabled_file"
  fi
}

case "$*" in
  '--user is-active --quiet gisland.service')
    [[ $(<"$active_file") == 1 ]]
    ;;
  '--user is-enabled gisland.service')
    enabled_state=$(<"$enabled_file")
    printf '%s\n' "$enabled_state"
    [[ $enabled_state == enabled ]]
    ;;
  '--user stop gisland.service')
    printf '0\n' >"$active_file"
    ;;
  '--user start gisland.service'|'--user restart gisland.service')
    printf '1\n' >"$active_file"
    ;;
  '--user enable gisland.service')
    printf 'enabled\n' >"$enabled_file"
    ;;
  '--user disable gisland.service')
    printf 'disabled\n' >"$enabled_file"
    ;;
  '--user enable --now gisland.service')
    printf 'enabled\n' >"$enabled_file"
    printf '1\n' >"$active_file"
    ;;
  '--user disable --now gisland.service')
    printf 'disabled\n' >"$enabled_file"
    printf '0\n' >"$active_file"
    ;;
esac
EOF

  cat >"$case_dir/bin/journalctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'journalctl|%s\n' "$*" >>"$TEST_COMMAND_LOG"
EOF

  cat >"$case_dir/bin/sleep" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'sleep|%s\n' "$*" >>"$TEST_COMMAND_LOG"
EOF

  cat >"$case_dir/bin/ninja" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

  cat >"$case_dir/bin/gislandctl-template" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'gislandctl|%s\n' "$*" >>"$TEST_COMMAND_LOG"
exit "${FAKE_HEALTH_STATUS:-0}"
EOF

  chmod +x "$case_dir/bin/"*
  printf '%s\n' "$case_dir"
}

run_installer() {
  local case_dir=$1
  shift
  env -u DISPLAY -u XAUTHORITY \
    HOME="$case_dir/home" \
    XDG_CONFIG_HOME="$case_dir/xdg-config" \
    XDG_DATA_HOME="$case_dir/xdg-data" \
    PATH="$case_dir/bin:/usr/bin:/bin" \
    TEST_COMMAND_LOG="$case_dir/commands.log" \
    FAKE_BIN_DIR="$case_dir/bin" \
    "$@" \
    bash "$source_dir/scripts/install-local.sh"
}

success_case=$(make_case success)
run_installer "$success_case" \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1 \
  DISPLAY=:55 XAUTHORITY="$success_case/Xauthority"
success_log="$success_case/commands.log"
assert_contains "$success_log" "cmake|--preset release -DCMAKE_INSTALL_PREFIX=$success_case/home/.local"
assert_contains "$success_log" 'cmake|--build --preset release'
assert_contains "$success_log" 'systemctl|--user stop gisland.service'
assert_contains "$success_log" 'cmake|--install build/release'
assert_contains "$success_log" 'systemctl|--user daemon-reload'
assert_contains "$success_log" 'systemctl|--user import-environment DISPLAY XAUTHORITY'
assert_contains "$success_log" 'systemctl|--user start gisland.service'
assert_contains "$success_log" 'systemctl|--user enable gisland.service'
assert_contains "$success_log" 'gislandctl|status'
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-audio"
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-audio-control"
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-clock-calendar"
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-battery"
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-notifications"
assert_contains "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-notification-history"
assert_contains "$success_log" \
  "rm|-f -- $success_case/home/.local/share/gisland/distributed/modules/clock-calendar/calendar.lua"
assert_contains "$success_log" \
  "rm|-rf -- $success_case/home/.local/share/gisland/audio/gisland_audio"
assert_contains "$success_log" \
  "rm|-rf -- $success_case/home/.local/share/gisland/distributed/modules/audio-lua"
assert_contains "$success_log" \
  "rm|-rf -- $success_case/home/.local/share/gisland/battery/gisland_battery"
assert_contains "$success_log" \
  "rm|-rf -- $success_case/home/.local/share/gisland/notifications/gisland_notifications"
assert_not_contains "$success_log" 'sudo'
assert_not_contains "$success_log" '/usr/local'
assert_service_state "$success_case" 1 enabled

build_line=$(line_number "$success_log" 'cmake|--build --preset release')
stop_line=$(line_number "$success_log" 'systemctl|--user stop gisland.service')
install_line=$(line_number "$success_log" 'cmake|--install build/release')
cleanup_line=$(line_number "$success_log" "rm|-f -- $success_case/home/.local/bin/gisland-audio")
start_line=$(line_number "$success_log" 'systemctl|--user start gisland.service')
health_line=$(line_number "$success_log" 'gislandctl|status')
enable_line=$(line_number "$success_log" 'systemctl|--user enable gisland.service')
((build_line < stop_line)) || fail 'the build must complete before the service stops'
((stop_line < install_line && install_line < cleanup_line && cleanup_line < start_line)) ||
  fail 'legacy cleanup must follow install while the service is stopped'
((start_line < health_line && health_line < enable_line)) ||
  fail 'the service must become healthy before it is enabled'
assert_not_exists "$success_case/home/.local/bin/gisland-audio"
assert_not_exists "$success_case/home/.local/bin/gisland-audio-control"
assert_not_exists "$success_case/home/.local/bin/gisland-clock-calendar"
assert_not_exists "$success_case/home/.local/bin/gisland-battery"
assert_not_exists "$success_case/home/.local/bin/gisland-notifications"
assert_not_exists "$success_case/home/.local/bin/gisland-notification-history"
assert_not_exists \
  "$success_case/home/.local/share/gisland/distributed/modules/clock-calendar/calendar.lua"
assert_not_exists "$success_case/home/.local/share/gisland/audio/gisland_audio"
assert_not_exists "$success_case/home/.local/share/gisland/distributed/modules/audio-lua"
assert_not_exists "$success_case/home/.local/share/gisland/battery/gisland_battery"
assert_not_exists "$success_case/home/.local/share/gisland/notifications/gisland_notifications"
assert_exists "$success_case/home/.local/bin/unrelated-tool"
[[ $(<"$success_case/home/.local/share/gisland/audio/sentinel") == keep-audio-sibling ]] ||
  fail 'legacy audio cleanup changed a sibling file'
[[ $(<"$success_case/home/.local/share/gisland/battery/sentinel") == keep-battery-sibling ]] ||
  fail 'legacy battery cleanup changed a sibling file'
[[ $(<"$success_case/home/.local/share/gisland/notifications/sentinel") == keep-notification-sibling ]] ||
  fail 'legacy notification cleanup changed a sibling file'
[[ $(<"$success_case/home/.local/state/gisland/notifications-history.json") == preserved-history ]] ||
  fail 'notification cleanup changed persisted history'
[[ $(<"$success_case/home/.config/gisland/config.toml") == keep-config ]] ||
  fail 'user configuration changed'
[[ $(<"$success_case/xdg-config/gisland/sentinel") == keep-xdg-config ]] ||
  fail 'XDG configuration changed'
[[ $(<"$success_case/xdg-data/gisland/sentinel") == keep-xdg-data ]] ||
  fail 'XDG data changed'
[[ $(<"$success_case/home/.local/share/gisland/modules/sentinel") == keep-module ]] ||
  fail 'user module changed'

run_installer "$success_case" \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1 \
  DISPLAY=:55 XAUTHORITY="$success_case/Xauthority"

build_failure_case=$(make_case build-failure)
if run_installer "$build_failure_case" FAKE_CMAKE_FAILURE=build FAKE_SERVICE_ACTIVE=1; then
  fail 'a build failure must fail the installer'
fi
assert_not_contains "$build_failure_case/commands.log" 'systemctl|--user is-active'
assert_not_contains "$build_failure_case/commands.log" 'systemctl|--user stop'
assert_not_contains "$build_failure_case/commands.log" 'rm|'
assert_exists "$build_failure_case/home/.local/bin/gisland-audio"
assert_exists "$build_failure_case/home/.local/bin/gisland-clock-calendar"
assert_exists "$build_failure_case/home/.local/bin/gisland-battery"
assert_exists "$build_failure_case/home/.local/bin/gisland-notifications"
assert_exists "$build_failure_case/home/.local/bin/gisland-notification-history"
assert_exists "$build_failure_case/home/.local/share/gisland/audio/gisland_audio/application.py"

install_failure_case=$(make_case install-failure)
if run_installer "$install_failure_case" FAKE_CMAKE_FAILURE=install \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'an install failure must fail the installer'
fi
assert_contains "$install_failure_case/commands.log" 'systemctl|--user stop gisland.service'
assert_contains "$install_failure_case/commands.log" 'systemctl|--user start gisland.service'
assert_not_contains "$install_failure_case/commands.log" 'rm|'
assert_exists "$install_failure_case/home/.local/bin/gisland-audio-control"
assert_exists \
  "$install_failure_case/home/.local/share/gisland/distributed/modules/audio-lua/module.toml"
assert_exists "$install_failure_case/home/.local/bin/gisland-clock-calendar"

replacement_failure_case=$(make_case replacement-failure)
if run_installer "$replacement_failure_case" FAKE_CMAKE_MISSING_AUDIO=1 \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'a missing replacement audio package must fail the installer'
fi
assert_contains "$replacement_failure_case/commands.log" 'systemctl|--user stop gisland.service'
assert_contains "$replacement_failure_case/commands.log" 'systemctl|--user start gisland.service'
assert_not_contains "$replacement_failure_case/commands.log" 'rm|'
assert_exists "$replacement_failure_case/home/.local/bin/gisland-audio"
assert_exists "$replacement_failure_case/home/.local/bin/gisland-audio-control"
assert_exists "$replacement_failure_case/home/.local/share/gisland/audio/gisland_audio/application.py"
[[ $(<"$replacement_failure_case/home/.local/share/gisland/distributed/modules/audio/module.toml") == stale-audio ]] ||
  fail 'a stale preseeded audio manifest must not pass replacement verification'
[[ $(<"$replacement_failure_case/home/.local/share/gisland/distributed/modules/audio/audio.lua") == stale-audio ]] ||
  fail 'a stale preseeded audio Lua package must not pass replacement verification'

host_freshness_failure_case=$(make_case host-freshness-failure)
printf 'stale-lua-host\n' \
  >"$host_freshness_failure_case/home/.local/bin/gisland-lua-host"
if run_installer "$host_freshness_failure_case" FAKE_CMAKE_MISSING_HOST=1 \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'a stale Lua host must fail replacement verification'
fi
assert_contains "$host_freshness_failure_case/commands.log" \
  'systemctl|--user stop gisland.service'
assert_contains "$host_freshness_failure_case/commands.log" \
  'systemctl|--user start gisland.service'
assert_not_contains "$host_freshness_failure_case/commands.log" 'rm|'
assert_exists "$host_freshness_failure_case/home/.local/bin/gisland-audio"
assert_exists "$host_freshness_failure_case/home/.local/bin/gisland-clock-calendar"
assert_exists "$host_freshness_failure_case/home/.local/bin/gisland-battery"
[[ $(<"$host_freshness_failure_case/home/.local/bin/gisland-lua-host") == stale-lua-host ]] ||
  fail 'a stale preseeded Lua host unexpectedly changed'
assert_service_state "$host_freshness_failure_case" 1 enabled

clock_replacement_failure_case=$(make_case clock-replacement-failure)
if run_installer "$clock_replacement_failure_case" FAKE_CMAKE_MISSING_CLOCK=1 \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'a missing replacement clock-calendar package must fail the installer'
fi
assert_contains "$clock_replacement_failure_case/commands.log" \
  'systemctl|--user stop gisland.service'
assert_contains "$clock_replacement_failure_case/commands.log" \
  'systemctl|--user start gisland.service'
assert_not_contains "$clock_replacement_failure_case/commands.log" 'rm|'
assert_exists "$clock_replacement_failure_case/home/.local/bin/gisland-clock-calendar"
[[ $(<"$clock_replacement_failure_case/home/.local/share/gisland/distributed/modules/clock-calendar/module.toml") == stale-clock ]] ||
  fail 'a stale preseeded clock-calendar manifest must not pass replacement verification'
[[ $(<"$clock_replacement_failure_case/home/.local/share/gisland/distributed/modules/clock-calendar/clock_calendar.lua") == stale-clock ]] ||
  fail 'a stale preseeded clock-calendar Lua package must not pass replacement verification'

battery_replacement_failure_case=$(make_case battery-replacement-failure)
if run_installer "$battery_replacement_failure_case" FAKE_CMAKE_MISSING_BATTERY=1 \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'a missing replacement battery package must fail the installer'
fi
assert_contains "$battery_replacement_failure_case/commands.log" \
  'systemctl|--user stop gisland.service'
assert_contains "$battery_replacement_failure_case/commands.log" \
  'systemctl|--user start gisland.service'
assert_not_contains "$battery_replacement_failure_case/commands.log" 'rm|'
assert_exists "$battery_replacement_failure_case/home/.local/bin/gisland-battery"
assert_exists \
  "$battery_replacement_failure_case/home/.local/share/gisland/battery/gisland_battery/application.py"
assert_service_state "$battery_replacement_failure_case" 1 enabled
[[ $(<"$battery_replacement_failure_case/home/.local/share/gisland/distributed/modules/battery/battery.lua") == stale-battery ]] ||
  fail 'a stale preseeded battery package must not pass replacement verification'
[[ $(<"$battery_replacement_failure_case/home/.local/share/gisland/battery/sentinel") == keep-battery-sibling ]] ||
  fail 'battery replacement failure changed an unrelated sibling'

notification_replacement_failure_case=$(make_case notification-replacement-failure)
if run_installer "$notification_replacement_failure_case" FAKE_CMAKE_MISSING_NOTIFICATIONS=1 \
  FAKE_SERVICE_ACTIVE=1 FAKE_SERVICE_ENABLED=1; then
  fail 'a missing replacement notifications package must fail the installer'
fi
assert_contains "$notification_replacement_failure_case/commands.log" \
  'systemctl|--user start gisland.service'
assert_not_contains "$notification_replacement_failure_case/commands.log" 'rm|'
assert_exists "$notification_replacement_failure_case/home/.local/bin/gisland-notifications"
assert_exists "$notification_replacement_failure_case/home/.local/bin/gisland-notification-history"
assert_exists \
  "$notification_replacement_failure_case/home/.local/share/gisland/notifications/gisland_notifications/application.py"
[[ $(<"$notification_replacement_failure_case/home/.local/share/gisland/distributed/modules/notifications/module.toml") == stale-notification-manifest ]] ||
  fail 'a stale preseeded notification manifest must not pass replacement verification'
[[ $(<"$notification_replacement_failure_case/home/.local/share/gisland/distributed/modules/notifications/notifications.lua") == stale-notification-module ]] ||
  fail 'a stale preseeded notification module must not pass replacement verification'
[[ $(<"$notification_replacement_failure_case/home/.local/state/gisland/notifications-history.json") == preserved-history ]] ||
  fail 'notification replacement failure changed persisted history'

for failure_kind in health cleanup; do
  for active in 0 1; do
    for enabled in 0 1; do
      failure_case=$(make_case "$failure_kind-$active-$enabled")
      if [[ $failure_kind == health ]]; then
        if run_installer "$failure_case" FAKE_HEALTH_STATUS=1 \
          FAKE_SERVICE_ACTIVE="$active" FAKE_SERVICE_ENABLED="$enabled"; then
          fail 'an unhealthy installed service must fail the installer'
        fi
        assert_contains "$failure_case/commands.log" \
          'journalctl|--user -u gisland.service --no-pager -n 20'
        assert_not_contains "$failure_case/commands.log" \
          'systemctl|--user import-environment'
      else
        if run_installer "$failure_case" FAKE_RM_FAIL_MATCH=gisland-audio-control \
          FAKE_SERVICE_ACTIVE="$active" FAKE_SERVICE_ENABLED="$enabled"; then
          fail 'a legacy cleanup failure must fail the installer'
        fi
        assert_not_exists "$failure_case/home/.local/bin/gisland-audio"
        assert_exists "$failure_case/home/.local/bin/gisland-audio-control"
        assert_exists \
          "$failure_case/home/.local/share/gisland/audio/gisland_audio/application.py"
      fi

      if [[ $enabled == 1 ]]; then
        assert_contains "$failure_case/commands.log" 'systemctl|--user enable gisland.service'
        expected_enabled=enabled
      else
        assert_contains "$failure_case/commands.log" 'systemctl|--user disable gisland.service'
        expected_enabled=disabled
      fi
      if [[ $active == 1 ]]; then
        assert_contains "$failure_case/commands.log" 'systemctl|--user start gisland.service'
      else
        assert_contains "$failure_case/commands.log" 'systemctl|--user stop gisland.service'
      fi
      assert_service_state "$failure_case" "$active" "$expected_enabled"
    done
  done
done

masked_failure_case=$(make_case masked-failure)
if run_installer "$masked_failure_case" FAKE_CMAKE_FAILURE=install \
  FAKE_SERVICE_ACTIVE=0 FAKE_SERVICE_ENABLED_STATE=masked; then
  fail 'an install failure with a masked service must fail the installer'
fi
assert_not_contains "$masked_failure_case/commands.log" 'systemctl|--user enable gisland.service'
assert_not_contains "$masked_failure_case/commands.log" 'systemctl|--user disable gisland.service'
assert_service_state "$masked_failure_case" 0 masked

printf 'install-local behavioral tests passed\n'
