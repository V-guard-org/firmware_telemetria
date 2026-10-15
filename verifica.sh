#!/bin/sh
# Every automated test, before each push (docs/TESTES.md §3.3).
set -e
cd "$(dirname "$0")"
pio test -e native
# The native firmware too: host/test_native_firmware.py runs it against the Decoder.
pio run -e native
python3 -m unittest discover -s host
