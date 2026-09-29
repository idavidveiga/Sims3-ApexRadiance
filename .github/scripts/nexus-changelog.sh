#!/usr/bin/env bash
# Turns a GitHub release body (markdown) into Nexus text: one line per change, no markdown marks, the "## Install"
# section left out (the Nexus text adds its own), and no backslashes (the Nexus page drops them: paths become "A > B").
# Reads stdin, writes stdout.
awk '/^## Install/ { exit } { print }' |
  sed -E -e 's/\*\*//g' -e 's/`//g' -e 's/^## (.*)$/\1:/' -e 's/^[-*] //' -e 's/^  [-*] /    /' \
         -e 's/[\\]([ .,;:)]|$)/\1/g' -e 's/[\\]/ > /g' |
  grep -v '^[[:space:]]*$' || true
