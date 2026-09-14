#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
cc -O1 -g -Wall -Wextra -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 -L/opt/homebrew/lib -Wno-deprecated-declarations \
  src/webp.c tests/webp.c -o /tmp/nuvio-webp-tests -lSDL2
/tmp/nuvio-webp-tests
