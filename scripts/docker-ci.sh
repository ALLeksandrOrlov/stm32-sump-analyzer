#!/bin/sh
# One local command mirrors the GitHub Actions container run.
set -eu
cd "$(dirname "$0")/.."
image=${CI_IMAGE:-stm32-sump-analyzer-ci:local}
docker buildx build --target ci --load -t "$image" .
revision=$(git rev-parse HEAD 2>/dev/null || printf unknown)
ref=$(git describe --tags --always 2>/dev/null || printf local)
if test -n "$(git status --porcelain 2>/dev/null)"; then dirty=true; else dirty=false; fi
docker run --rm --network=none --user "$(id -u):$(id -g)" \
  --mount "type=bind,src=$PWD,dst=/project" \
  -e SOURCE_REVISION="$revision" -e SOURCE_REF="$ref" -e SOURCE_DIRTY="$dirty" \
  "$image" python3 scripts/ci.py "$@"
