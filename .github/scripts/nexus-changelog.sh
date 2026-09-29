#!/usr/bin/env bash
# Turns a GitHub release body (markdown) into Nexus changelog text: one line per change, no markdown marks, the
# "## Install" section left out (the Nexus page says how to install). Reads stdin, writes stdout.
awk '/^## Install/ { exit } { print }' |
  sed -E -e 's/\*\*//g' -e 's/`//g' -e 's/^## (.*)$/\1:/' -e 's/^[-*] //' -e 's/^  [-*] /    /' |
  grep -v '^[[:space:]]*$' || true
