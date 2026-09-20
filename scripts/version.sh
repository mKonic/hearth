#!/usr/bin/env bash
# The one place a hearth version number comes from.
#
#   code   10000 + N   N = commits on HEAD. Monotonic, never hand-maintained.
#   name   git describe --tags --always --dirty
#
# 0 and "unknown", not a plausible-looking number, when git cannot answer: a fallback that reads
# like a real version produces mismatch reports nobody can interpret.
#
#   ./scripts/version.sh        -> "10004 v1.0.0"
#   ./scripts/version.sh code   -> "10004"
#   ./scripts/version.sh name   -> "v1.0.0"
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"

code=0
name="unknown"

if git -C "$HERE" rev-parse --git-dir >/dev/null 2>&1; then
    # A shallow clone counts wrong and would silently produce a *lower* number than the previous
    # build. Say so rather than emit one.
    if [ -f "$(git -C "$HERE" rev-parse --git-dir)/shallow" ]; then
        echo "version.sh: shallow clone -- 'git fetch --unshallow' for a real build number" >&2
    else
        commits=$(git -C "$HERE" rev-list --count HEAD 2>/dev/null || echo 0)
        [ "$commits" -gt 0 ] && code=$((10000 + commits))
    fi
    name=$(git -C "$HERE" describe --tags --always --dirty 2>/dev/null || echo unknown)
fi

case "${1:-both}" in
    code) echo "$code" ;;
    name) echo "$name" ;;
    *)    echo "$code $name" ;;
esac
