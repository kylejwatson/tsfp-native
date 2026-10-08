#!/bin/bash
# Apply this project's changes to the xboxrecomp submodule (idempotent).
cd "$(dirname "$0")/xboxrecomp" || exit 1
for p in ../patches/*.patch; do
  if git apply --check --reverse "$p" 2>/dev/null; then echo "already applied: $p"
  else git apply "$p" && echo "applied: $p"; fi
done
