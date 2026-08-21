#!/usr/bin/env bash
# Stage driver wrapping ahls-exec.
set -euo pipefail

AHLS=${AHLS:-/p/psg/swip/w/renlizha/installed_apptainer}
WORKDIR=${WORKDIR:-$(cd "$(dirname "$0")/.." && pwd)}
STAGE=emu
DESIGN=repetition
DEVICE=${DEVICE:-Agilex7}
VARIANT=default

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage) STAGE=$2; shift 2 ;;
    --design) DESIGN=$2; shift 2 ;;
    --variant) VARIANT=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

exec_in() {
  "$AHLS/bin/ahls-exec" --bind "$WORKDIR" --pwd "$WORKDIR" -- "$@"
}

case "$STAGE" in
  emu)
    echo "WORKDIR=$WORKDIR design=$DESIGN stage=emu variant=$VARIANT"
    mkdir -p "$WORKDIR/build/emu"
    case "$VARIANT" in
      default)
        exec_in ahls -Wall -DFPGA_EMULATOR -g -O0 -I device -I host \
          host/relay_bp_host.cpp test/relay_bp_tb.cpp \
          -o build/emu/relay_bp.fpga_emu
        exec_in ./build/emu/relay_bp.fpga_emu \
          test/golden/repetition_code_relay_i32.json
        ;;
      multileg)
        # Same sources; only pre_iter overridden so leg 0 fails on
        # error_qubit_1 and the relay transition (reset msgs / carry M) runs.
        exec_in ahls -Wall -DFPGA_EMULATOR -g -O0 -DKPRE_ITER_OVERRIDE=1 \
          -I device -I host \
          host/relay_bp_host.cpp test/relay_bp_tb.cpp \
          -o build/emu/relay_bp_multileg.fpga_emu
        exec_in ./build/emu/relay_bp_multileg.fpga_emu \
          test/golden/repetition_code_relay_multileg_i32.json
        ;;
      float)
        # Legacy float32 golden / -DRELAY_MSG_T_FLOAT build.
        exec_in ahls -Wall -DFPGA_EMULATOR -DRELAY_MSG_T_FLOAT -g -O0 \
          -I device -I host \
          host/relay_bp_host.cpp test/relay_bp_tb.cpp \
          -o build/emu/relay_bp_float.fpga_emu
        exec_in ./build/emu/relay_bp_float.fpga_emu \
          test/golden/repetition_code_relay.json
        ;;
      *)
        echo "unknown --variant '$VARIANT' (expected default|multileg|float)" >&2
        exit 2
        ;;
    esac
    ;;
  report)
    echo "WORKDIR=$WORKDIR design=$DESIGN stage=report DEVICE=$DEVICE variant=$VARIANT"
    mkdir -p "$WORKDIR/build/report"
    case "$VARIANT" in
      default)
        EXTRA_FLAGS=()
        ;;
      latency)
        # Step 5 (kernel latency optimization plan): BP iterations are
        # inherently sequential (no cross-iteration pipelining possible),
        # so the default throughput-oriented scheduler settings mostly
        # cost area/fMAX for no throughput benefit here -- try the minimum
        # latency flow instead (handbook 16_2_1).
        EXTRA_FLAGS=(-Xsoptimize=latency)
        ;;
      *)
        echo "unknown --variant '$VARIANT' for stage=report (expected default|latency)" >&2
        exit 2
        ;;
    esac
    exec_in ahls -Wall -DFPGA_HARDWARE -I device -I host \
      -Xshardware -Xsdevice="$DEVICE" -fsycl-link=early "${EXTRA_FLAGS[@]}" \
      host/relay_bp_host.cpp test/relay_bp_tb.cpp \
      -o build/report/relay_bp.report
    python3 "$WORKDIR/scripts/parse_report.py" "$WORKDIR/build/report"
    ;;
  sim|fpga|analyze)
    echo "stage '$STAGE' is not implemented yet (later phase)" >&2
    exit 2
    ;;
  *)
    echo "usage: $0 --stage {emu|report|sim|fpga|analyze} [--design repetition] [--variant default|multileg]" >&2
    exit 2
    ;;
esac
