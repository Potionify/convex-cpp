#!/usr/bin/env sh
# Wrapper for bump_version.cmake. Usage: bump_version.sh major|minor|patch|x.y.z
set -eu
if [ "$#" -ne 1 ]; then
    echo "Usage: $(basename "$0") major|minor|patch|x.y.z" >&2
    exit 1
fi
exec cmake -DBUMP="$1" -P "$(dirname "$0")/bump_version.cmake"
