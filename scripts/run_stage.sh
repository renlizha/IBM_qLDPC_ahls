#!/usr/bin/env bash
# Stage driver wrapping ahls-exec.
set -euo pipefail

AHLS=${AHLS:-/p/psg/swip/w/renlizha/installed_apptainer}
WORKDIR=${WORKDIR:-$(cd "$(dirname "$0")/.." && pwd)}
STAGE=emu
DESIGN=repetition
DEVICE=${DEVICE:-Agilex7}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage) STAGE=$2; shift 2 ;;
    --design) DESIGN=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

exec_in() {
  "$AHLS/bin/ahls-exec" --bind "$WORKDIR" --pwd "$WORKDIR" -- "$@"
}

case "$STAGE" in
  emu)
    echo "WORKDIR=$WORKDIR design=$DESIGN stage=emu"
    mkdir -p "$WORKDIR/build/emu"
    exec_in ahls -Wall -DFPGA_EMULATOR -g -O0 -I device -I host \
      host/min_sum_bp_host.cpp test/min_sum_bp_tb.cpp \
      -o build/emu/min_sum_bp.fpga_emu
    exec_in ./build/emu/min_sum_bp.fpga_emu \
      test/golden/repetition_code_relay.json
    ;;
  report)
    echo "WORKDIR=$WORKDIR design=$DESIGN stage=report DEVICE=$DEVICE"
    mkdir -p "$WORKDIR/build/report"
    exec_in ahls -Wall -DFPGA_HARDWARE -I device -I host \
      -Xshardware -Xsdevice="$DEVICE" -fsycl-link=early \
      host/min_sum_bp_host.cpp test/min_sum_bp_tb.cpp \
      -o build/report/min_sum_bp.report
    python3 "$WORKDIR/scripts/parse_report.py" "$WORKDIR/build/report"
    ;;
  sim|fpga|analyze)
    echo "stage '$STAGE' is not implemented yet (later phase)" >&2
    exit 2
    ;;
  *)
    echo "usage: $0 --stage {emu|report|sim|fpga|analyze} [--design repetition]" >&2
    exit 2
    ;;
esac
