# AHLS Relay-BP Decoder

SYCL / Altera HLS (`ahls`) port of IBM’s **Relay-BP-SS** quantum LDPC decoder
([arXiv:2506.01779](https://arxiv.org/abs/2506.01779), Appendix D Algorithm 1),
starting with the **3-qubit repetition code**.

The device kernel is a traceable mapping of the paper’s pseudocode and the
Rust/Python golden model in `../rust/relay` (`RelayDecoder` /
`RelayDecoderI32` by default, `RelayDecoderF32` with `-DRELAY_MSG_T_FLOAT`) —
not a reinterpretation.

## Current status (2026-08-21)

| Item | State |
|------|--------|
| Algorithm | Full Relay-BP-SS, `kNumLegs=4`, `S=1` (`NConv{stop_after:1}`) |
| Default numerics | **`MsgT=int`**, `data_scale S=256` (matches `RelayDecoderI32`) |
| Float path | `-DRELAY_MSG_T_FLOAT` + float goldens (`run_stage.sh --variant float`) |
| Kernel structure | Flattened `it` loop, latched `done`, full outer `#pragma unroll`, fused init/HD, deg-2 check specialization, algebraic V2C, `kernel_args_restrict` |
| `fpga_emu` | **4/4 PASS** default i32, multileg i32, and float variants |
| HLS `report` | Agilex7; `--variant latency` → est. **~89.4k ALUTs / 68.7k FFs / 42 RAMs / 0 DSPs / 445 MLABs**, scheduler fMAX target **480 MHz** (pre-Quartus-fit) |
| FPGA `sim` | Manual `ahls -Xssimulation` + MPSIM; RTL co-sim via **Questa** (not Quartus post-fit sim). With `-Xsoptimize=latency` + pinned fMAX: avg full-kernel **~698 ns @ 480 MHz**, **~1333 ns @ 100 MHz** (see below) |
| `fpga` / P&R | Not wired in `run_stage.sh` yet |

**Latency reading tip:** \(t_{\mathrm{ns}} = N_{\mathrm{cycles}} / f_{\mathrm{MAX(MHz)}} \times 1000\). Lower fMAX can cut cycles but raise ns — judge by time at a credible clock, not cycle count alone.

Optimization process, measurement recipe, and fMAX tables:
[`docs/latency_optimization.md`](docs/latency_optimization.md).  
Step-by-step history: [`docs/analysis_log.md`](docs/analysis_log.md).

## Layout

```
ahls/
├── device/                 # SYCL kernel + types
│   ├── relay_bp_types.hpp
│   ├── relay_bp_kernels.hpp
│   └── leg_gamma_table.gen.hpp   # generated γ table (do not hand-edit)
├── host/                   # queue, launch, priors
│   ├── relay_bp_host.hpp
│   └── relay_bp_host.cpp
├── test/
│   ├── relay_bp_tb.cpp
│   └── golden/             # JSON oracles from relay_bp
├── scripts/
│   ├── run_stage.sh        # emu / report driver (ahls-exec)
│   ├── export_golden_vectors.py
│   └── parse_report.py
├── docs/                   # env notes, design flow, analysis + opt docs
└── build/                  # emu / report / sim outputs (gitignored)
```

## Prerequisites

| Item | Notes |
|------|--------|
| AHLS Apptainer | `ahls` only runs **inside** the container via `ahls-exec` |
| Working tree bind | `--bind` + `--pwd` to this repo (see `docs/env_setup_notes.md`) |
| Device family | Default `-Xsdevice=Agilex7` (this install has no Agilex9 HLS family yet) |
| Python venv (goldens) | `ahls/.venv` with `relay_bp`, `numpy`, `scipy` |
| Sim extras | Quartus + Questa inside the container; `SALT_LICENSE_SERVER` as needed |

Default paths used by scripts (override with env vars if needed):

```bash
export AHLS=/p/psg/swip/w/renlizha/installed_apptainer
export WORKDIR=/p/psg/swip/w/renlizha/ai_project/IBM/ahls   # or: cd ahls && pwd
```

Full CLI patterns for emu / report / sim / fpga: [`docs/env_setup.md`](docs/env_setup.md).
Machine-specific working commands: [`docs/env_setup_notes.md`](docs/env_setup_notes.md).

## Quick start

### Emulator (functional)

Production i32 (`pre_iter=10`):

```bash
./scripts/run_stage.sh --stage emu
# → build/emu/relay_bp.fpga_emu
# → test/golden/repetition_code_relay_i32.json  (expect 4/4 PASS)
```

Multileg regression (`-DKPRE_ITER_OVERRIDE=1`, forces leg 1 on `error_qubit_1`):

```bash
./scripts/run_stage.sh --stage emu --variant multileg
# → build/emu/relay_bp_multileg.fpga_emu
# → test/golden/repetition_code_relay_multileg_i32.json
# → expect error_qubit_1 iterations=3
```

Float32 golden path:

```bash
./scripts/run_stage.sh --stage emu --variant float
```

### HLS report (area / schedule / fMAX target)

```bash
./scripts/run_stage.sh --stage report --variant latency
# → build/report/relay_bp.report (+ .prj/)
# HTML: build/report/relay_bp.report.prj/reports/report.html
# Uses -Xsoptimize=latency
```

### FPGA simulator (RTL latency; manual)

`run_stage.sh --stage sim` is not wired yet. Working pattern:

```bash
# Compile (example: latency opt, Agilex7)
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -Xssimulation -DFPGA_SIMULATOR -I device -I host \
  -Xssimulation -Xsghdl -Xsdevice=Agilex7 -Xsoptimize=latency \
  host/relay_bp_host.cpp test/relay_bp_tb.cpp \
  -o build/sim/relay_bp.fpga_sim

# Optional fMAX pin: add -Xsclock=480MHz and temporarily
# [[intel::scheduler_target_fmax_mhz(480)]] on the single_task lambda.

# Run (MPSIM + Questa backend)
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR/build/sim" -- \
  env CL_CONTEXT_MPSIM_DEVICE_INTELFPGA=1 \
  ./relay_bp.fpga_sim ../../test/golden/repetition_code_relay_i32.json
```

Parse cycles from
`build/sim/relay_bp.fpga_sim.prj/reports/resources/json/{sim_stats,simulation_raw}.ndjson`.
Details: [`docs/latency_optimization.md`](docs/latency_optimization.md).

## Decoder config (production)

| Parameter | Value |
|-----------|--------|
| Code | 3-qubit repetition, \(H=\begin{bmatrix}1&1&0\\0&1&1\end{bmatrix}\) |
| Numeric type | `int` (i32), `S=256`, matching `RelayDecoderI32` |
| Legs \(R\) | 4 (leg 0 Mem-BP + 3 relay sets) |
| \(T_0\) / \(T_{r\ge1}\) | `kPreIter=10` / `kSetMaxIter=10` |
| Solutions \(S\) | 1 (`NConv{stop_after:1}`; no weight keep-lowest) |
| \(\gamma_0\) | 0.1 (uniform on leg 0); Q-scaled in `kLegGammaQ` |
| Legs 1–3 \(\gamma\) | `Uniform(-0.24, 0.66)`, seed 0 → `leg_gamma_table.gen.hpp` |
| Priors (i32 golden) | `trunc(S · ln((1-p)/p))` → 1486 for the repetition vectors |

Algorithm mapping lives in `device/relay_bp_kernels.hpp` (comments cite
pseudocode lines and Rust `RelayDecoder::{decode_detailed,decode_inner,init_next_set}`).

## Golden vectors

Regenerate (writes JSON + `device/leg_gamma_table.gen.hpp`):

```bash
source .venv/bin/activate
python scripts/export_golden_vectors.py --skip-fixed-point
```

| File | Role |
|------|------|
| `test/golden/repetition_code_relay_i32.json` | **Default** production oracle (`RelayDecoderI32`, S=256) |
| `test/golden/repetition_code_relay_multileg_i32.json` | `pre_iter=1` leg-transition regression (i32) |
| `test/golden/repetition_code_relay.json` | Float32 production oracle |
| `test/golden/repetition_code_relay_multileg.json` | Float32 multileg regression |
| `test/golden/repetition_code.json` | Historical Q5.2 plain BP artifact |
| `test/golden/repetition_code_dmem.json` | Historical Q5.2 DMem artifact |

Golden model source of truth: `../rust/relay` (read-only in this workflow).

## Docs

| Doc | Contents |
|-----|----------|
| [`docs/latency_optimization.md`](docs/latency_optimization.md) | **Optimization approach**, sim/report metrics, fMAX tables |
| [`docs/analysis_log.md`](docs/analysis_log.md) | Chronological config vs emu / report / sim |
| [`docs/env_setup.md`](docs/env_setup.md) | Generic `ahls-exec` recipes (emu / report / sim / fpga) |
| [`docs/env_setup_notes.md`](docs/env_setup_notes.md) | Working paths, decoder commands |
| [`docs/qldpc-agentic-design-flow.md`](docs/qldpc-agentic-design-flow.md) | Agentic AGX9 pipeline overview |

## Related trees (read-only references)

- `../rust/relay` — IBM Relay-BP Rust + Python bindings (golden model)
- `../stella/products/source/acl/` — compiler sources (background only; do not edit)
