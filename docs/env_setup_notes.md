# AHLS env / invocation notes (Phase 0 smoke test)

Recorded from the Phase 0 `fpga_emu` smoke test. These are the working
commands; copy them rather than re-deriving bind/pwd flags.

## Install + project roots

| Item | Path |
|------|------|
| AHLS install (contains `bin/ahls-exec`) | `/p/psg/swip/w/renlizha/installed_apptainer` |
| This project (`WORKDIR`) | `/p/psg/swip/w/renlizha/ai_project/IBM/ahls` |
| Device family (later flows) | `-Xsdevice=Agilex7` (this install has no Agilex9 HLS family) |

`ahls` only exists **inside** the Apptainer. Always wrap with `ahls-exec`.

## Working emulator commands

```bash
AHLS=/p/psg/swip/w/renlizha/installed_apptainer
WORKDIR=/p/psg/swip/w/renlizha/ai_project/IBM/ahls

# Compile (one shot)
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -DFPGA_EMULATOR -g -O0 \
  build/smoke_test/smoke_test.cpp \
  -o build/smoke_test/smoke_test.fpga_emu

# Run
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ./build/smoke_test/smoke_test.fpga_emu
```

**Result:** `Running on device: Altera(R) FPGA Emulation Device` then `PASSED`.

The `ac_fixed` probe used the same pattern with
`build/smoke_test/ac_fixed_probe.cpp` and also `PASSED`.

## Bind / pwd mechanics

- `--bind "$WORKDIR"` makes the project tree visible in the container.
- `--pwd "$WORKDIR"` sets the working directory, so relative source and
  output paths (and later `test/golden/*.json`) resolve inside the container.
- Do **not** call `ahls` on the host (`command not found`).

## Fixed-point types available in the container

Confirmed during the Phase 0 probe (do not guess these):

- Header: `#include <sycl/ext/altera/ac_types/ac_fixed.hpp>`
- FPGA selectors: `#include <sycl/ext/altera/fpga_extensions.hpp>`
  (`sycl::ext::altera::fpga_emulator_selector_v` is an alias of the intel
  selector).
- Type: `ac_fixed<W, I, S, Q=AC_TRN, O=AC_WRAP>`
- Q5.2 signed as `ac_fixed<7, 5, true>`: W=7, I=5, 2 fractional bits.
- Host-side construction from `double` is supported.
  `ac_fixed<7,5,true>(5.806135)` became **5.75** (default `AC_TRN`; quantum 0.25).
- `to_double()` works on the host for printing/debug.
- Mentor signed `ac_fixed<7,5,true>` range is **[-16, 15.75]**. The Rust
  `set_fixed(5, 2)` clip bound is `max_data_value = 16.0`, which is *not*
  representable in that 7-bit type (would wrap with `AC_WRAP`). The decoder
  therefore uses `ac_fixed<8, 6, true>` (still 2 fractional bits) so ±16.0
  can be stored, then clips to ±16 to match the golden model.

## Device selector macros (from env_setup.md)

```cpp
#if FPGA_SIMULATOR
  auto selector = sycl::ext::altera::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
  auto selector = sycl::ext::altera::fpga_selector_v;
#else
  auto selector = sycl::ext::altera::fpga_emulator_selector_v;
#endif
```

Compile emulator with `-DFPGA_EMULATOR`. Kernel style is full SYCL
(`queue` + `h.single_task`, buffers), not HLS `component` functions.

## Phase 1 decoder emulator commands

```bash
AHLS=/p/psg/swip/w/renlizha/installed_apptainer
WORKDIR=/p/psg/swip/w/renlizha/ai_project/IBM/ahls

$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -DFPGA_EMULATOR -g -O0 -I device -I host \
  host/relay_bp_host.cpp test/relay_bp_tb.cpp \
  -o build/emu/relay_bp.fpga_emu

$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ./build/emu/relay_bp.fpga_emu \
  test/golden/repetition_code_relay_i32.json
```

Or: `ahls/scripts/run_stage.sh --stage emu` (production `pre_iter=10`, i32).

Multileg regression (`pre_iter=1`, forces leg 1 on `error_qubit_1`):

```bash
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -DFPGA_EMULATOR -g -O0 -DKPRE_ITER_OVERRIDE=1 \
  -I device -I host \
  host/relay_bp_host.cpp test/relay_bp_tb.cpp \
  -o build/emu/relay_bp_multileg.fpga_emu

$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ./build/emu/relay_bp_multileg.fpga_emu \
  test/golden/repetition_code_relay_multileg_i32.json
```

Or: `ahls/scripts/run_stage.sh --stage emu --variant multileg`

Float32: `run_stage.sh --stage emu --variant float` (`-DRELAY_MSG_T_FLOAT`).

Compile is one `ahls` line with both `.cpp` files (same one-shot pattern for
report / sim / fpga: all sources on that flow’s single `ahls` line). Output is
`build/emu/relay_bp.fpga_emu` (or `_multileg` / `_float`). The second
`ahls-exec` only **runs** that binary.

**Result:** 4/4 production i32 + 4/4 multileg i32 + 4/4 float under
`fpga_emu`. Multileg `error_qubit_1` reports `iters=3` (entered leg 1).

Regenerate goldens + `device/leg_gamma_table.gen.hpp` (needs `ahls/.venv`):

```bash
source .venv/bin/activate
python scripts/export_golden_vectors.py --skip-fixed-point
```

## Relay-BP-SS (Algorithm 1) + default i32 (S=256)

Kernel implements the full outer relay loop (`kNumLegs=4`: leg 0 Mem-BP with
uniform `gamma0=0.1`, then 3 relay legs with per-node γ from
`leg_gamma_table.gen.hpp`) and DMem-BP bias matching Rust
`compute_variable_prior`. Default `MsgT` is **`int`** with
`data_scale_value=S=256` (`RelayDecoderI32`); float32 via
`-DRELAY_MSG_T_FLOAT`. No magnitude clip. `S=1` (stop on first converged
leg). `kPreIter` is overridable via `-DKPRE_ITER_OVERRIDE=N` for the multileg
regression.

Latency optimization / sim measurement:
[`latency_optimization.md`](latency_optimization.md). Status table:
[`../README.md`](../README.md).

Golden files:

| File | Config | Oracle | fpga_emu |
|------|--------|--------|----------|
| `test/golden/repetition_code_relay_i32.json` | RelayDecoderI32, S=256, pre_iter=10 | default production | 4/4 PASS |
| `test/golden/repetition_code_relay_multileg_i32.json` | same, pre_iter=1 | leg-transition proof | 4/4 PASS (`error_qubit_1` iters=3) |
| `test/golden/repetition_code_relay.json` | RelayDecoderF32, pre_iter=10 | float path | 4/4 PASS (`--variant float`) |
| `test/golden/repetition_code_relay_multileg.json` | float, pre_iter=1 | float multileg | 4/4 PASS |
| `test/golden/repetition_code.json` | historical Q5.2 plain BP | Rust `decode_repetition_code_int` | (artifact) |
| `test/golden/repetition_code_dmem.json` | historical Q5.2 DMem γ=0.1 | none | (artifact) |
