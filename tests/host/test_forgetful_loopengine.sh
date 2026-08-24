#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_forgetful_loopengine"
mkdir -p "$(dirname "$bin")"

cc -std=c11 -Wall -Wextra -Werror \
  -Isrc \
  tests/host/test_forgetful_loopengine.c \
  src/modules/audio_fx/forgetful/dsp/forgetful.c \
  -lm \
  -o "$bin"

"$bin"
