# AHLS Relay-BP Decoder

SYCL / Altera HLS (`ahls`) port of IBM’s **Relay-BP-SS** quantum LDPC decoder
([arXiv:2506.01779](https://arxiv.org/abs/2506.01779), Appendix D Algorithm 1),
starting with the **3-qubit repetition code**.

The device kernel is a traceable mapping of the paper’s pseudocode and the
Rust/Python golden model in `../rust/relay` (`RelayDecoder` /
`relay_bp.RelayDecoderF32`) — not a reinterpretation.

**Current status:** float32 Relay-BP-SS (`kNumLegs=4`, `S=1`), bit-exact vs
golden vectors under `fpga_emu`; HLS `report` flow runs on Agilex7. The
per-BP-iteration kernel loop has been restructured (flattened `leg`×`t` loop,
no data-dependent `break`s, unrolled degree-bounded sub-loops, algebraic
prefix-sum removal, `kernel_args_restrict`) so it is now reported **pipelined
at II=15** (was not pipelined at all before), giving an estimated **~31 ns
per BP iteration** at the compiler's ~480 MHz Agilex7 scheduling *target*
(down from a ~508 ns/iteration baseline; note this is a pre-Quartus-fit
report-flow estimate, not a placed-and-routed measurement — see
[`docs/analysis_log.md`](docs/analysis_log.md) for the full derivation,
caveats, and a `scheduler_target_fmax_mhz` sweep). Fixed-point/`int4`
numerics (which would shorten this further) remain out of scope for this
float32 line of work.

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
├── docs/                   # env notes, design flow, analysis log
└── build/                  # emu binaries + report outputs (gitignored)
```

## Prerequisites

| Item | Notes |
|------|--------|
| AHLS Apptainer | `ahls` only runs **inside** the container via `ahls-exec` |
| Working tree bind | `--bind` + `--pwd` to this repo (see `docs/env_setup_notes.md`) |
| Device family | Default `-Xsdevice=Agilex7` (this install has no Agilex9 HLS family yet) |
| Python venv (goldens) | `ahls/.venv` with `relay_bp`, `numpy`, `scipy` |

Default paths used by scripts (override with env vars if needed):

```bash
export AHLS=/p/psg/swip/w/renlizha/installed_apptainer
export WORKDIR=/p/psg/swip/w/renlizha/ai_project/IBM/ahls   # or: cd ahls && pwd
```

Full CLI patterns for emu / report / sim / fpga: [`docs/env_setup.md`](docs/env_setup.md).
Machine-specific working commands: [`docs/env_setup_notes.md`](docs/env_setup_notes.md).

## Quick start

### Emulator (functional)

Production config (`pre_iter=10`):

```bash
./scripts/run_stage.sh --stage emu
# → build/emu/relay_bp.fpga_emu
# → test/golden/repetition_code_relay.json  (expect 4/4 PASS)
```

Multileg regression (`-DKPRE_ITER_OVERRIDE=1`, forces leg 1 on `error_qubit_1`):

```bash
./scripts/run_stage.sh --stage emu --variant multileg
# → build/emu/relay_bp_multileg.fpga_emu
# → expect error_qubit_1 iterations=3
```

### HLS report (area / schedule / fMAX)

```bash
./scripts/run_stage.sh --stage report
# → build/report/relay_bp.report (+ .prj/)
# HTML: build/report/relay_bp.report.prj/reports/report.html

# Minimum-latency scheduler flow (`-Xsoptimize=latency`); no measured
# II/Fmax benefit over default for this kernel, but a free, no-downside
# area trim -- see docs/analysis_log.md.
./scripts/run_stage.sh --stage report --variant latency
```

`sim` / `fpga` stages are not wired yet (later phase).

## Decoder config (production)

| Parameter | Value |
|-----------|--------|
| Code | 3-qubit repetition, \(H=\begin{bmatrix}1&1&0\\0&1&1\end{bmatrix}\) |
| Numeric type | `float` (IEEE-754), matching `RelayDecoderF32` |
| Legs \(R\) | 4 (leg 0 Mem-BP + 3 relay sets) |
| \(T_0\) / \(T_{r\ge1}\) | `kPreIter=10` / `kSetMaxIter=10` |
| Solutions \(S\) | 1 (`NConv{stop_after:1}`; no weight keep-lowest) |
| \(\gamma_0\) | 0.1 (uniform on leg 0) |
| Legs 1–3 \(\gamma\) | `Uniform(-0.24, 0.66)`, seed 0 → `leg_gamma_table.gen.hpp` |

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
| `test/golden/repetition_code_relay.json` | Production Relay-BP-SS oracle |
| `test/golden/repetition_code_relay_multileg.json` | `pre_iter=1` leg-transition regression |
| `test/golden/repetition_code.json` | Historical Q5.2 plain BP artifact |
| `test/golden/repetition_code_dmem.json` | Historical Q5.2 DMem artifact |

Golden model source of truth: `../rust/relay` (read-only in this workflow).

## Docs

| Doc | Contents |
|-----|----------|
| [`docs/env_setup.md`](docs/env_setup.md) | Generic `ahls-exec` recipes (emu / report / sim / fpga) |
| [`docs/env_setup_notes.md`](docs/env_setup_notes.md) | Working paths, decoder emu/report commands |
| [`docs/analysis_log.md`](docs/analysis_log.md) | Config vs `fpga_emu` / report history |
| [`docs/qldpc-agentic-design-flow.md`](docs/qldpc-agentic-design-flow.md) | Agentic AGX9 pipeline overview |

## Related trees (read-only references)

- `../rust/relay` — IBM Relay-BP Rust + Python bindings (golden model)
- `../stella/products/source/acl/` — compiler sources (background only; do not edit)
