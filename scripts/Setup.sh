#!/usr/bin/env bash
# hearth dev setup: fetch submodules, generate project files.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> Fetching submodules"
git submodule update --init --recursive

echo "==> Generating gmake project files"
premake5 gmake

echo
echo "Done. Build with:"
echo "   make config=debug -j4"
