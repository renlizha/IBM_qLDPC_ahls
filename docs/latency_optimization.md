# Latency optimization approach

How we optimize and measure the AHLS Relay-BP kernel on the 3-qubit
repetition code. Chronological step log: [`analysis_log.md`](analysis_log.md).

## Goal

Minimize **wall-clock decode latency** (ns per kernel invocation / per BP
iteration) while staying bit-exact vs the Rust golden
(`RelayDecoderI32` by default, `RelayDecoderF32` via `-DRELAY_MSG_T_FLOAT`).

Primary metric:

\[
t_{\mathrm{ns}} = \frac{N_{\mathrm{cycles}}}{f_{\mathrm{MAX\,(MHz)}}} \times 1000
\]

Cycle count alone is not enough: a lower fMAX target often shrinks cycles
but can **increase** ns if frequency falls faster than \(N\).

## Flows we use (and what they mean)

| Flow | Command shape | What it gives | What it is not |
|------|---------------|----------------|----------------|
| **emu** | `run_stage.sh --stage emu` | Functional bit-exactness (fast) | No RTL timing |
| **report** | `--stage report [--variant latency]` | Area estimate, schedule, *scheduler* fMAX target (`-fsycl-link=early`) | Not Quartus fit; `kernel clock: TBD` |
| **sim** | `ahls -Xssimulation …` + MPSIM run | RTL cycle latency via Questa backend | Not post-P&R timing sim |

**Sim stack:** SYCL FPGA simulator device (`aclmsim0` / MPSIM) drives generated
RTL; **Questa** (`questa_fse` in the container) is the HDL engine. This is
**not** “Quartus Simulator” / post-fit NativeLink. Quartus tooling helps build
the sim project; place-and-route is the separate `-Xshardware` / `fpga` path
(not wired in `run_stage.sh` yet).

Preferred compile flags for latency experiments:

- `-Xsoptimize=latency`
- `-Xsdevice=Agilex7`
- Optional clock pin: `-Xsclock=<N>MHz` **and** temporary
  `[[intel::scheduler_target_fmax_mhz(N)]]` on the `single_task` lambda
  (revert the attribute after the build so the tree is not stuck on one
  target).

## Measurement recipe (sim)

1. Build under a dedicated folder (e.g. `build_fmax480/sim`, `build/sim`).
2. Run with `CL_CONTEXT_MPSIM_DEVICE_INTELFPGA=1` and the matching golden
   (`repetition_code_relay_i32.json` by default).
3. Parse:
   - `…/reports/resources/json/sim_stats.ndjson` — min/max/avg **cycles**
   - `…/simulation_raw.ndjson` — per-call `start`/`end`;  
     \(\mathrm{cycles}=(end-start)/\mathrm{period}\)
   - `schedule.ndjson` — stall-free **core** cluster latency (compute-heavy
     region; better for “is Relay-BP faster?” than full kernel wrapper)
4. Convert ns with the **pinned / reported** fMAX, not the tracker’s default
   `period=1000` ps alone. MPSIM often keeps `period=1000` as the time base
   even when `-Xsclock=480MHz`; treat that as cycle units, then
   `ns = cycles × (1000 / fMAX_MHz)`.

Host `steady_clock` prints around `submit`/`wait` are wall time of the sim
process — useful for sanity, **not** kernel RTL latency.

## Optimization process (what we did)

Work was incremental: **diagnose → change structure → re-measure → keep only
what moved the metric**. Functional goldens (emu, then sim) after each step.

### Phase A — Make the outer iteration schedulable (float32 era)

Baseline (`report`): nested `leg`/`t` loops with data-dependent `break` →
**not pipelined**; estimated ~508 ns/BP-iter @ 480 MHz target; ~31k ALUTs.

| Step | Change | Why |
|------|--------|-----|
| 1–2 | Flatten `leg`×`t` → one `it` loop over `kTotalIter`; no `break`; latch `done` for writeback | Static trip count; remove early-exit that blocks pipelining |
| 2 refine | Replace `if(cond){ for(...) }` with unconditional loops + ternary selects | Avoid “conditional loop” 0-or-N trip counts |
| 3 | `#pragma unroll` degree-bounded loops; V2C as `sum − own` (drop prefix-sum `running`) | Kill II=3 / loop-carried deps on check/var edges |
| 4 | Min/second-min without `FLT_MAX` sentinel; fold HD/syndrome into unrolled block | Suspected limiter; **II stayed 15** (float dot-product still dominated) |
| 5 | `[[intel::kernel_args_restrict]]`; opt-in `-Xsoptimize=latency` | Area/ordering hygiene; latency flag free area trim |

**Result then:** outer body **pipelined II=15** → ≈31 ns/iter @ 480 MHz
*report target*; area ~7.4k ALUTs. `scheduler_target_fmax_mhz` sweep showed
II rising with target; left **unpinned** in shipped source until fit validates
clock.

### Phase B — Push latency further (current kernel)

| Step | Change | Effect |
|------|--------|--------|
| Outer `#pragma unroll` on `it` | Fully unroll `kTotalIter=40` | Removes loop overhead; **fixed worst-case** compute (early success does not shorten datapath) |
| Loop fusion + deg-2 check specialization | Init/reset/HD fused; check node specialized for `kMaxChkDeg==2` | Single-BB-friendly schedule; fewer nested loops |
| **Numeric: `MsgT=int`, S=256** | Match `RelayDecoderI32` (`data_scale_value=256`); γ Q-table; divide-first mix | Bit-exact i32 goldens; **0 DSPs** in recent report (was float DSP-heavy) |

Trade-off of full outer unroll: great for worst-case / comparison tables;
typical goldens only need 1–2 BP iters, so a **pipelined (non-unrolled) flat
`it` loop** is the likely next latency win for average-case.

### Phase C — Measure with RTL sim + fMAX pins

Same i32 kernel, `-Xsoptimize=latency`, MPSIM + Questa:

| Build | fMAX pin | Full-kernel avg cycles | Avg ns @ pin | Core stall-free |
|-------|----------|------------------------:|-------------:|----------------:|
| `build/sim` (default tracker ~1 ns/cyc) | (family / ~480 est.) | ~353 | *see caveat* | 237 cyc |
| `build_fmax480` | 480 MHz | ~335 | **~698** | 237 cyc → ~494 ns |
| `build_fmax100` | 100 MHz | ~133 | **~1333** | 33 cyc → 330 ns |

**How to read this:** lowering fMAX reduced cycles but **raised** wall ns
(100 MHz vs 480 MHz). Prefer **ns at a credible fMAX** over raw cycle count.
Default sim `period=1000` must not be mistaken for “design closes at 1 GHz.”

## Current kernel shape (checklist)

- Flattened `it` loop, latched `done`, no `break`
- Outer `#pragma unroll` over `kTotalIter`
- Algebraic V2C; deg-2 specialized checks; fused init/HD paths
- `kernel_args_restrict` on `single_task`
- Default `MsgT = int`, `kDataScale = 256`; float via `-DRELAY_MSG_T_FLOAT`
- Report: `./scripts/run_stage.sh --stage report --variant latency`

## Open next steps

1. Try **removing outer `#pragma unroll`** to restore early-exit / II-style
   average latency while keeping the flat `it` structure.
2. Wire `run_stage.sh --stage sim` (today: manual `ahls -Xssimulation` + MPSIM).
3. Quartus **fit** (`fpga` / `-Xshardware`) to validate achieved clock vs
   scheduler targets.
4. Scale beyond 3-qubit repetition (area/latency will change with degree/N).
