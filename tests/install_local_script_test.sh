#!/usr/bin/env bash

set -euo pipefail

source_dir=${1:?usage: install_local_script_test.sh SOURCE_DIR}
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT

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
    "$case_dir/home/.local/share/gisland/modules"
  printf 'keep-config\n' >"$case_dir/home/.config/gisland/config.toml"
  printf 'keep-module\n' >"$case_dir/home/.local/share/gisland/modules/sentinel"
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
fi
EOF

  cat >"$case_dir/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'systemctl|%s\n' "$*" >>"$TEST_COMMAND_LOG"
if [[ $* == '--user is-active --quiet gisland.service' ]]; then
  [[ ${FAKE_SERVICE_ACTIVE:-0} == 1 ]]
fi
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
    PATH="$case_dir/bin:/usr/bin:/bin" \
    TEST_COMMAND_LOG="$case_dir/commands.log" \
    FAKE_BIN_DIR="$case_dir/bin" \
    "$@" \
    bash "$source_dir/scripts/install-local.sh"
}

success_case=$(make_case success)
run_installer "$success_case" \
  FAKE_SERVICE_ACTIVE=1 DISPLAY=:55 XAUTHORITY="$success_case/Xauthority"
success_log="$success_case/commands.log"
assert_contains "$success_log" "cmake|--preset release -DCMAKE_INSTALL_PREFIX=$success_case/home/.local"
assert_contains "$success_log" 'cmake|--build --preset release'
assert_contains "$success_log" 'systemctl|--user stop gisland.service'
assert_contains "$success_log" 'cmake|--install build/release'
assert_contains "$success_log" 'systemctl|--user daemon-reload'
assert_contains "$success_log" 'systemctl|--user import-environment DISPLAY XAUTHORITY'
assert_contains "$success_log" 'systemctl|--user enable --now gisland.service'
assert_contains "$success_log" 'gislandctl|status'
assert_not_contains "$success_log" 'sudo'
assert_not_contains "$success_log" '/usr/local'

build_line=$(line_number "$success_log" 'cmake|--build --preset release')
stop_line=$(line_number "$success_log" 'systemctl|--user stop gisland.service')
((build_line < stop_line)) || fail 'the build must complete before the service stops'
[[ $(<"$success_case/home/.config/gisland/config.toml") == keep-config ]] ||
  fail 'user configuration changed'
[[ $(<"$success_case/home/.local/share/gisland/modules/sentinel") == keep-module ]] ||
  fail 'user module changed'

run_installer "$success_case" \
  FAKE_SERVICE_ACTIVE=1 DISPLAY=:55 XAUTHORITY="$success_case/Xauthority"

build_failure_case=$(make_case build-failure)
if run_installer "$build_failure_case" FAKE_CMAKE_FAILURE=build FAKE_SERVICE_ACTIVE=1; then
  fail 'a build failure must fail the installer'
fi
assert_not_contains "$build_failure_case/commands.log" 'systemctl|--user is-active'
assert_not_contains "$build_failure_case/commands.log" 'systemctl|--user stop'

install_failure_case=$(make_case install-failure)
if run_installer "$install_failure_case" FAKE_CMAKE_FAILURE=install FAKE_SERVICE_ACTIVE=1; then
  fail 'an install failure must fail the installer'
fi
assert_contains "$install_failure_case/commands.log" 'systemctl|--user stop gisland.service'
assert_contains "$install_failure_case/commands.log" 'systemctl|--user start gisland.service'

health_failure_case=$(make_case health-failure)
if run_installer "$health_failure_case" FAKE_HEALTH_STATUS=1 FAKE_SERVICE_ACTIVE=0; then
  fail 'an unhealthy installed service must fail the installer'
fi
assert_contains "$health_failure_case/commands.log" \
  'journalctl|--user -u gisland.service --no-pager -n 20'
assert_not_contains "$health_failure_case/commands.log" 'systemctl|--user import-environment'

printf 'install-local behavioral tests passed\n'
