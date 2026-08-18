#!/usr/bin/env bash
#
# run_tests.sh — run every host-side test. No hardware required.
#
# The C tests build the firmware's own svf.c against a simulated MAX V TAP,
# so they exercise the real parser rather than a copy of it. That is the
# point: a test against a reimplementation would pass while the firmware
# was broken.
#
# Usage:  cd tests && ./run_tests.sh

set -u
cd "$(dirname "$0")"

fail=0
run() {
    printf '\n\033[1m== %s ==\033[0m\n' "$1"
    shift
    if ! "$@"; then
        fail=1
        printf '\033[91mFAILED\033[0m\n'
    fi
}

# The shim stands in for the Pico SDK's timing calls so firmware sources
# compile on the host unchanged.
mkdir -p pico
echo '#include "shim.h"' > pico/stdlib.h

printf 'Building simulated TAP...\n'
gcc -c -w -I. -I../firmware sim_jtag.c -o sim_jtag.o || exit 1

printf 'Building parser tests...\n'
gcc -Wall -I. -I../firmware -include shim.h -o test \
    test_main.c ../firmware/svf.c sim_jtag.o shim.c || exit 1

run "SVF parser (simulated MAX V)" ./test

# Optional: parse a real SVF end to end. TDO vectors are stripped first,
# because the simulated TAP cannot produce real programming data — this
# checks the PARSER against a real file, not the device model.
if [ -f sample.svf ]; then
    gcc -w -I. -I../firmware -include shim.h -o realtest \
        real_test.c ../firmware/svf.c sim_jtag.o shim.c || exit 1
    sed -E 's/ TDO \([0-9A-Fa-f]*\)//g; s/ MASK \([0-9A-Fa-f]*\)//g' \
        sample.svf > /tmp/notdo.svf
    run "Real SVF file" ./realtest /tmp/notdo.svf
else
    printf '\n\033[2m(skipping real-SVF test: drop a sample.svf here to enable)\033[0m\n'
fi

if command -v node >/dev/null 2>&1; then
    run "Intel HEX parser" node hex_test.js
else
    printf '\n\033[2m(skipping HEX parser test: node not installed)\033[0m\n'
fi

if command -v python3 >/dev/null 2>&1 && python3 -c 'import serial' 2>/dev/null; then
    run "CLI protocol" python3 test_protocol.py
else
    printf '\n\033[2m(skipping CLI test: needs python3 and pyserial)\033[0m\n'
fi

printf '\n'
if [ "$fail" -eq 0 ]; then
    printf '\033[92mAll tests passed.\033[0m\n'
else
    printf '\033[91mSome tests failed.\033[0m\n'
fi
exit "$fail"
