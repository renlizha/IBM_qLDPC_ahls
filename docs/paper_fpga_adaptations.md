# Maurer et al. FPGA adaptations (current AHLS)

Reference: Maurer et al., *Real-time decoding of the gross code memory with FPGA*,
[arXiv:2510.21600](https://arxiv.org/abs/2510.21600) (paper FPGA / HDL mesh).

Algorithm / golden model remains IBM Relay-BP-SS
([arXiv:2506.01779](https://arxiv.org/abs/2506.01779)) and `../rust/relay`.
This note lists **only** what AHLS currently took from the **Maurer FPGA paper**,
and what it deliberately has **not** adopted yet.

## Adapted (in code today)

| Paper idea | Where in AHLS | Notes |
|------------|---------------|--------|
| **int4.2.8** messages | `device/relay_bp_types.hpp` | `N=4` mag bits + explicit sign; prior scale `S=2`; memory scale `M=8`; `β=(1−γ)`, `βint=⌊β·M⌋` → `kLegBetaInt` |
| **Edge messages** as sign + magnitude | `EdgeMsg { neg, mag }` | Matches paper μ/ν style (not float LLRs on the wire) |
| **CNU / VNU units** | `check_node_unit` / `variable_node_unit` in `relay_bp_decode.hpp` | Named after paper Fig. 3a/b: min/2nd-min + sign XOR (CNU); exclusive-sum VNU + Relay/Mem mix |
| **Flooding schedule** | `bp_iteration_flood` | All checks then all variables each BP iteration (software flood) |
| **α ≈ 1 − 2^(−t)** style damping | `apply_alpha` | Shift-based approximation of paper α schedule |
| **SAT into message domain** | `sat_mag` / `sat_post` | Bound magnitudes to 4-bit range |
| **Λ mix with integer β** | `relay_mix` + `mul_shift_add` | Paper-style `(1−β)·prior + β·m_prev` in `M=8` fixed scale (HLS-friendly shift-add) |

Code entry points: types → `relay_bp_types.hpp`; decode body → `relay_bp_decode.hpp`;
SYCL launch → `relay_bp_kernels.hpp`.

## Not adapted yet (paper FPGA vs our schedule)

| Paper FPGA | Current AHLS |
|------------|--------------|
| Hand HDL **mesh** of CNU/VNU blocks + wiring | One SYCL `single_task` + nested software loops |
| **True 2-cycle** CNU→VNU stage split (registers on message wires; ~2 clocks / BP iter) | Software BP-iter carry: one `for (t)` trip does full CNU+VNU; HLS pipelines that loop (II=1 @ ~66 MHz af) |
| Gross / large qLDPC graph | Hardcoded **3-qubit repetition** graph |
| Target device / 24 ns/iter claim (VU19P, etc.) | Agilex7 **report** estimates only; not paper parity |

Rust `../rust/relay` is the **same software flood model** as AHLS (CNU then VNU inside `run_iteration`) — also **not** the paper’s 2-cycle mesh.

## Throughput context (why the gap matters)

- Paper: ~**24 ns / BP iteration** via 2 clocks × high fMAX mesh.
- AHLS (best so far): B2 **II=1**, af≈**66 MHz** → ~**15 ns/iter** if clocked at af; schedule fMAX target 480 MHz is not achieved on the feedback path.
- Sprinkling `fpga_reg` on the software carry raised II (13–17) and **hurt** ns/iter; see `docs/analysis_log.md`.

Closing the architectural gap means an explicit CNU|VNU **phase split** (or equivalent), not more registers on the current `for (t)` body.

## Related docs

- [`analysis_log.md`](analysis_log.md) — experiment history
- [`../README.md`](../README.md) — project status / config table
- [`qldpc-agentic-design-flow.md`](qldpc-agentic-design-flow.md) — AGX9 agentic pipeline
