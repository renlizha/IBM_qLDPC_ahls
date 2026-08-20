#!/usr/bin/env bash
# Build host int4 oracle and emit committed golden JSON files.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/build" "$ROOT/test/golden"

CXX=${CXX:-g++}
common=(-O2 -std=c++17 -Wall -Wextra -DRELAY_BP_HOST_ORACLE -I "$ROOT/device")

echo "export production int4 golden (pre_iter=10)"
"$CXX" "${common[@]}" "$ROOT/host/export_int4_goldens.cpp" \
  -o "$ROOT/build/export_int4_goldens"
"$ROOT/build/export_int4_goldens" \
  "$ROOT/test/golden/repetition_code_relay_int4.json"

echo "export multileg int4 golden (pre_iter=1 / KPRE_ITER_OVERRIDE)"
"$CXX" "${common[@]}" -DKPRE_ITER_OVERRIDE=1 "$ROOT/host/export_int4_goldens.cpp" \
  -o "$ROOT/build/export_int4_goldens_multileg"
"$ROOT/build/export_int4_goldens_multileg" \
  "$ROOT/test/golden/repetition_code_relay_multileg_int4.json"

echo "done"
