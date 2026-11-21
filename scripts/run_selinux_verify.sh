#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/run_selinux_verify.py" "$@"
