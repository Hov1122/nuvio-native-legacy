#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
flags=(-O1 -g -Wall -Wextra -Isrc -Wno-macro-redefined -Wno-deprecated-declarations)
if [ "${SANITIZE:-0}" = 1 ]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
cc "${flags[@]}" src/simkl.c src/js.c src/jsw.c tests/simkl.c -o /tmp/nuvio-simkl-tests
/tmp/nuvio-simkl-tests
