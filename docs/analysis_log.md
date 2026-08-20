# Analysis log

Running record of decoder config vs functional / (later) AGX9 metrics.

| Date | Config | Notes | fpga_emu |
|------|--------|-------|----------|
| 2026-08-19 | Plain min-sum BP, Q5.2 (`ac_fixed<8,6,true>`), `gamma0=None`, max_iter=10, repetition `[[1,1,0],[0,1,1]]` | Phase 1 bring-up | 4/4 `repetition_code.json` PASS |
| 2026-08-19 | Same + DMem-BP Eq. 4, compile-time `kGamma = {0.1, 0.1, 0.1}` | Uniform gamma matching `gamma0=0.1`. Q5.2 truncates 0.1 → 0.00 (quantum 0.25), so messages match plain BP; multiply-add is in the kernel. `repetition_code_dmem.json` is `cross_checked: false` (no Rust oracle for `gamma0!=None` on this code). | 4/4 `repetition_code.json` PASS; 4/4 `repetition_code_dmem.json` PASS |
| 2026-08-20 | Full Relay-BP-SS (Algorithm 1), float32, `kNumLegs=4` (leg0 γ=0.1 + 3 explicit γ rows), `pre_iter=set_max_iter=10`, `S=1` | Scope: single DMem-BP leg → full outer relay loop (`RelayDecoder::decode_detailed`). Numeric: Q5.2 `ac_fixed` → `float` so γ∈(-0.24,0.66) is representable (Q5.2 had quantized γ0=0.1→0). Gamma table: `np.random.default_rng(0).uniform(-0.24,0.66,(3,3))` → `leg_gamma_table.gen.hpp` / `repetition_code_relay.json` (legs 1–3 use Rust `set_idx % num_sets` indexing). S=1 omits weight keep-lowest. **Verification gap:** all 4 syndromes converge within leg 0, so legs 1–3 reset/carry are unexercised; close in Phase 6 (d=7 / Gross). Historical `repetition_code.json` / `_dmem.json` left as fixed-point artifacts. | 4/4 `repetition_code_relay.json` PASS (decoding, detectors, success, total iters bit-exact vs `RelayDecoderF32`) |
