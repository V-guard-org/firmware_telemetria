#!/bin/sh
# Every automated test, before each push (docs/TESTES.md §3.3).
set -e
cd "$(dirname "$0")"
python3 -m unittest discover -s host
