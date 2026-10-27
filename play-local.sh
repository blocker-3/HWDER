#!/usr/bin/env bash
# All generated game code, compiler output, saves and caches stay local.
set -eu
task_source_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
exec python3 "$task_source_dir/tools/first_run.py" --launch "$@"
