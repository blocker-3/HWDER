#!/bin/sh
# Maintainer helper: copy this checkout's tracked files (with submodules) to a folder on a Linux test machine.
#   sh tools/linux/sync-to-ally.sh <ssh-host> <remote-dir>
set -eu
HOST=${1:?usage: sync-to-ally.sh <ssh-host> <remote-dir>}
DEST=${2:?usage: sync-to-ally.sh <ssh-host> <remote-dir>}
cd "$(dirname "$0")/../.."
{ git ls-files -z --recurse-submodules; git ls-files -z --others --exclude-standard; } | tar --null -T - -czf - | ssh -o BatchMode=yes "$HOST" \
    "rm -rf '$DEST.new' && mkdir -p '$DEST.new' && tar -xzf - -C '$DEST.new' && rm -rf '$DEST' && mv '$DEST.new' '$DEST'"
echo "synced $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- || echo '+dirty') to $HOST:$DEST"
