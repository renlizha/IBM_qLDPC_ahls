// Min-Sum Relay-BP-SS decode kernel for the 3-variable / 2-check repetition
// code. Maps Algorithm 1 (arXiv:2506.01779v2 Appendix D) and
// rust/relay RelayDecoder::decode_detailed / decode_inner / init_next_set.
// v1: one single_task kernel; pipe-connected check/variable kernels deferred.

#ifndef RELAY_BP_KERNELS_HPP
#define RELAY_BP_KERNELS_HPP

#include "relay_bp_types.hpp"

#include <sycl/sycl.hpp>

#include <chrono>
#include <iostream>

class MinSumBPDecodeID;

// Full Relay-BP-SS (S=1): outer legs r=0..R-1, inner DMem-BP iterations.
inline void min_sum_bp_decode(const DetectorWord &det_in,
                              const MsgT priors[kNVar], DecodeWord &out) {
  MsgT v2c[kNChk][kNVar];
  MsgT c2v[kNChk][kNVar];
  MsgT posterior[kNVar];
  MsgT posterior_prev[kNVar];

  // Pseudocode line 1 (leg-0-only full init) + first line-3:
  // Rust: MinSumBPDecoder::initialize_decoder (memory strengths via gamma0
  // are compile-time kLegGamma[0]; M_j(0) <- lambda_j; v2c <- lambda; c2v <- 0).
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
      v2c[c][v] = 0.0f;
      c2v[c][v] = 0.0f;
    }
  }
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    posterior[v] = 0.0f;
    posterior_prev[v] = priors[v];  // M_j(0) = lambda_j
#pragma unroll
    for (int i = 0; i < kVarDeg[v]; ++i) {
      v2c[kVarChecks[v][i]][v] = priors[v];
    }
  }

  unsigned char decoding[kNVar] = {0, 0, 0};
  unsigned char decoded_det[kNChk] = {0, 0};
  unsigned char decoding_out[kNVar] = {0, 0, 0};
  unsigned char decoded_det_out[kNChk] = {0, 0};
  bool success = false;
  bool done = false;
  int total_iters = 0;
  const MsgT alpha = 1.0f;

  // Steps 1-2 (kernel latency optimization plan): the original code had two
  // nested loops (`leg` x `t`) each exited via a data-dependent
  // `if (success) break;`. Per handbook 5_2_1 ("Avoid Complex Loop Exit
  // Conditions" / "Convert Nested Loops into a Single Loop"), that is
  // flattened here into one well-formed loop with a compile-time-constant
  // trip count and no `break`; `leg`/`t`/`is_leg_start` are derived from the
  // flat index `it` inside the loop body, and only the write-back
  // (`decoding_out`/`decoded_det_out`/`success`/`total_iters`) is gated by a
  // latched `done` flag once the syndrome first matches (pseudocode lines
  // 2-25 / RelayDecoder::decode_detailed + decode_inner).
  constexpr int kTotalIter = kPreIter + (kNumLegs - 1) * kSetMaxIter;
#pragma unroll
  for (int it = 0; it < kTotalIter; ++it) {
    int leg;
    int t;
    if (it < kPreIter) {
      leg = 0;
      t = it;
    } else {
      const int rest = it - kPreIter;
      leg = 1 + rest / kSetMaxIter;
      t = rest % kSetMaxIter;
    }
    const bool is_leg_start = (t == 0);

    // Pseudocode line 3 for legs r>=1. Rust: init_next_set (gamma via
    // kLegGamma[leg]) + initialize_check_to_variable /
    // initialize_variable_to_check. Do NOT reset posterior_prev — it
    // carries M_j(t) from the previous leg (pseudocode line 23 / the relay
    // mechanism).
    //
    // Step 2 refinement: an `if (cond) { for (...) {...} }` block is still
    // a "conditional loop" (the loop's own trip count looks data-dependent
    // -- 0 or kNChk*kNVar -- to the scheduler, which was enough on its own
    // to keep the whole flattened `it` loop out of pipelining even after
    // removing the `break`s). Per "Avoid Conditional Loops" (5_2_1),
    // rewritten as an unconditional loop with a per-element ternary select
    // instead, so the loop's trip count is always the same regardless of
    // `do_reset`.
    const bool do_reset = is_leg_start && leg > 0;
#pragma unroll
    for (int c = 0; c < kNChk; ++c) {
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        v2c[c][v] = do_reset ? 0.0f : v2c[c][v];
        c2v[c][v] = do_reset ? 0.0f : c2v[c][v];
      }
    }
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
#pragma unroll
      for (int i = 0; i < kMaxVarDeg; ++i) {
        if (i < kVarDeg[v]) {
          const int c = kVarChecks[v][i];
          v2c[c][v] = do_reset ? priors[v] : v2c[c][v];
        }
      }
    }

    {
      // Check -> variable (Eq 1 / compute_check_to_variable). Step 3: the
      // `i < deg` loops are bounded by the compile-time constant
      // `kMaxChkDeg`, so fully unroll them (guarded by `i < deg` for codes
      // where actual degree is < kMaxChkDeg) per handbook 12_5 Unroll
      // Loops -- this turns each sub-loop's own pipeline fill/drain
      // overhead, paid on every outer BP iteration, into feed-forward
      // combinational logic. The outer `c`/`v` loops below are also
      // unrolled (kNChk/kNVar are tiny compile-time constants too), so
      // that none of these loops are left as "sibling" loop nodes with
      // seemingly-inconsistent trip counts next to the conditional
      // (now ternary-selected) blocks above -- see Step 2 refinement note.
#pragma unroll
      for (int c = 0; c < kNChk; ++c) {
        const int deg = kChkDeg[c];
        MsgT messages[kMaxChkDeg];
        bool acc_sign = (det_in.d[c] == 1);
#pragma unroll
        for (int i = 0; i < kMaxChkDeg; ++i) {
          if (i < deg) {
            messages[i] = v2c[c][kChkVars[c][i]];
            acc_sign = acc_sign ^ (messages[i] < 0.0f);
          }
        }

        // Step 4: track "no second-min yet" with a 1-bit flag instead of
        // comparing against the kMsgAbsSentinel = FLT_MAX constant -- a
        // 1-cycle int/bool compare instead of a full-width float compare
        // on the min/second-min feedback chain.
        MsgT min_m = fabsf(messages[0]);
        MsgT second_m = 0.0f;
        bool has_second = false;
#pragma unroll
        for (int i = 1; i < kMaxChkDeg; ++i) {
          if (i < deg) {
            const MsgT a = fabsf(messages[i]);
            if (a < min_m) {
              second_m = min_m;
              has_second = true;
              min_m = a;
            } else if (!has_second || a < second_m) {
              second_m = a;
              has_second = true;
            }
          }
        }

        const MsgT out_min = alpha * min_m;
        const MsgT out_second = alpha * second_m;

#pragma unroll
        for (int i = 0; i < kMaxChkDeg; ++i) {
          if (i < deg) {
            const int v = kChkVars[c][i];
            const bool sign = acc_sign ^ (messages[i] < 0.0f);
            const MsgT mag =
                (fabsf(messages[i]) == min_m) ? out_second : out_min;
            c2v[c][v] = sign ? -mag : mag;
          }
        }
      }

      // Variable -> check (Eq 2/3) with DMem-BP bias (Eq 4 / line 5):
      //   Lambda_j(t) = (1-gamma_j(r))*lambda_j + gamma_j(r)*M_j(t-1)
      // No magnitude clip: RelayDecoderF32 leaves max_data_value unset.
      // Step 3: the original two-pass forward/backward prefix sum over
      // `running` created a loop-carried floating-point-add dependency
      // (measured II=3 in the baseline report). Algebraically,
      // v2c[c][v] = (full sum over all checks touching v) - c2v[c][v]
      // (the extrinsic message excludes check c's own contribution), so
      // compute the full sum once via an unrolled reduction, then derive
      // each v2c[c][v] independently -- no loop-carried dependency at all,
      // per 12_1/12_2 (Refactor / Relax the Loop-Carried Data Dependency).
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        const int deg = kVarDeg[v];
        const MsgT gamma_v = kLegGamma[leg][v];
        const MsgT lambda_j =
            priors[v] * (1.0f - gamma_v) + posterior_prev[v] * gamma_v;

        MsgT sum = lambda_j;
#pragma unroll
        for (int i = 0; i < kMaxVarDeg; ++i) {
          if (i < deg) {
            sum += c2v[kVarChecks[v][i]][v];
          }
        }
        posterior[v] = sum;

#pragma unroll
        for (int i = 0; i < kMaxVarDeg; ++i) {
          if (i < deg) {
            const int c = kVarChecks[v][i];
            v2c[c][v] = sum - c2v[c][v];
          }
        }
      }

#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        posterior_prev[v] = posterior[v];  // M_j(t-1) <- M_j(t)
      }

      // Pseudocode line 9 (HD) + line 10 (H*e == sigma).
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        decoding[v] = (posterior[v] <= 0.0f) ? 1 : 0;
      }

      // Step 4 (shannonization): fold the syndrome XOR-reduce into the same
      // fully-unrolled block that produces `decoding[v]` (both `c` and the
      // inner `i < kChkDeg[c]` loop are unrolled), instead of a separate,
      // only-partially-unrolled second pass over kNChk -- less residual
      // sequential logic sits in the tail of the per-iteration critical
      // path feeding the `done` latch (12_9 Shannonization).
      bool iter_success = true;
#pragma unroll
      for (int c = 0; c < kNChk; ++c) {
        unsigned char syn = 0;
#pragma unroll
        for (int i = 0; i < kMaxChkDeg; ++i) {
          if (i < kChkDeg[c]) {
            syn ^= decoding[kChkVars[c][i]];
          }
        }
        decoded_det[c] = syn;
        if (syn != det_in.d[c]) {
          iter_success = false;
        }
      }

      // Latch the write-back only while not yet done (pseudocode line 18 /
      // lines 21-22, S=kStopNConv=1; weight-comparison / keep-lowest-weight
      // for S>1, lines 12,14-17, remains a deliberate omission). Once
      // `done` is set, later flattened iterations keep computing (so the
      // loop stays well-formed / statically bounded) but no longer affect
      // the output — equivalent to the original nested `break`s. As above,
      // the write-back loops are kept unconditional (per-element ternary
      // select on `done`) rather than guarded by `if (!done) { for(...) }`,
      // to avoid reintroducing a "conditional loop".
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        decoding_out[v] = done ? decoding_out[v] : decoding[v];
      }
#pragma unroll
      for (int c = 0; c < kNChk; ++c) {
        decoded_det_out[c] = done ? decoded_det_out[c] : decoded_det[c];
      }
      if (!done) {
        success = iter_success;
        ++total_iters;
        done = iter_success;
      }
    }
  }

#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    out.decoding[v] = decoding_out[v];
  }
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
    out.decoded_detectors[c] = decoded_det_out[c];
  }
  out.success = success ? 1 : 0;
  out.iterations = static_cast<unsigned char>(total_iters);
}

inline void launch_min_sum_bp(sycl::queue &q, const DetectorWord &det_in,
                              const MsgT priors[kNVar], DecodeWord &out) {
  sycl::buffer<DetectorWord, 1> det_buf{&det_in, sycl::range(1)};
  sycl::buffer<MsgT, 1> prior_buf{priors, sycl::range(kNVar)};
  sycl::buffer<DecodeWord, 1> out_buf{&out, sycl::range(1)};

  // Host wall-clock around submit + wait: covers enqueue through kernel
  // completion and buffer write-back (not the report-flow cycle estimate).
  const auto t_start = std::chrono::steady_clock::now();
  q.submit([&](sycl::handler &h) {
    sycl::accessor det_acc{det_buf, h, sycl::read_only};
    sycl::accessor prior_acc{prior_buf, h, sycl::read_only};
    sycl::accessor out_acc{out_buf, h, sycl::write_only, sycl::no_init};
    // Step 5: det_acc/prior_acc/out_acc are distinct buffers, so it is safe
    // to tell the compiler they never alias -- this removes false
    // inter-accessor ordering hardware (handbook 20_6 FPGA Kernel
    // Attributes).
    //
    // A [[intel::scheduler_target_fmax_mhz(N)]] sweep (480/600/800/1000
    // MHz) was also tried here (see docs/analysis_log.md): II grows
    // sub-linearly with the target (15/17/18/23 cycles respectively), so
    // higher targets keep lowering the derived ns/iteration in this
    // "-fsycl-link=early" report flow. But that flow never runs the
    // Quartus placement/fit step (report.html's Quartus Fitter section
    // reports "TBD"), so these are unvalidated *targets*, not measured
    // achieved clocks -- pinning an optimistic one here would just make
    // the compiler spend area chasing a number this flow cannot confirm.
    // Left at the family default rather than fabricating a specific
    // "achieved" Fmax; revisit once the `fpga`/`sim` stages (real
    // Quartus fit) are wired up.
    h.single_task<MinSumBPDecodeID>([=]() [[intel::kernel_args_restrict]] {
      MsgT local_priors[kNVar];
      for (int v = 0; v < kNVar; ++v) {
        local_priors[v] = prior_acc[v];
      }
      DecodeWord local_out;
      min_sum_bp_decode(det_acc[0], local_priors, local_out);
      out_acc[0] = local_out;
    });
  });
  q.wait();
  const auto t_end = std::chrono::steady_clock::now();
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
          .count();
  std::cout << "single_task wall time: " << elapsed_ns << " ns ("
            << (elapsed_ns / 1000.0) << " us)\n";
}

#endif
