#!/usr/bin/env bash
# Runs once after the dev container was created (both configurations).
set -euo pipefail

# The workspace is a bind mount whose owner may differ from the container
# user (for example with rootless Docker); worktrees live elsewhere.
git config --global --add safe.directory '*'

# A shallow single-branch clone only fetches main; see CLAUDE.md.
if [ "$(git config --get-all remote.origin.fetch || true)" = \
     "+refs/heads/main:refs/remotes/origin/main" ]; then
    git remote set-branches origin '*'
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
idf.py --version
clang-format --version
node --version
cmake --version | head -n 1

if ls /dev/serial/by-id/* >/dev/null 2>&1; then
    ls -l /dev/serial/by-id/
fi
