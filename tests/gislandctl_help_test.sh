#!/usr/bin/env bash

set -euo pipefail

gislandctl=$1

help_option=$(env -u XDG_RUNTIME_DIR "$gislandctl" --help)
help_command=$(env -u XDG_RUNTIME_DIR "$gislandctl" help)

if [[ "$help_option" != "$help_command" ]]; then
  printf '%s\n' 'gislandctl help output differs between --help and help' >&2
  exit 1
fi

for expected in \
  'Usage: gislandctl <command> [options]' \
  'open' \
  'close' \
  'toggle' \
  'status [--json]' \
  'modules' \
  'reload' \
  'module restart <instance>' \
  'activate <instance> [--duration <duration>]' \
  'dismiss <context>' \
  'help' \
  '--help'; do
  if [[ "$help_option" != *"$expected"* ]]; then
    printf 'gislandctl help output is missing: %s\n' "$expected" >&2
    exit 1
  fi
done
