#!/bin/sh
# Make git run tools/githooks/pre-commit (the private-data scan) before every
# commit in this clone. Run it once after cloning; it changes only this
# clone's .git/config (core.hooksPath).
#   tools/install_hooks.sh
set -eu
cd "$(dirname "$0")/.."
git config core.hooksPath tools/githooks
echo "git hooks: $(git config core.hooksPath) (pre-commit runs tools/check_private.py --staged)"
