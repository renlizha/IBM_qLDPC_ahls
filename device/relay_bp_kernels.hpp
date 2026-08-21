// Min-Sum Relay-BP-SS decode kernel for the 3-variable / 2-check repetition
// code. Maps Algorithm 1 (arXiv:2506.01779v2 Appendix D) and
// rust/relay RelayDecoder::decode_detailed / decode_inner / init_next_set.
// v1: one single_task kernel; pipe-connected check/variable kernels deferred.
// Message type is templated (default MsgT from relay_bp_types.hpp = int).
// Loop fusion: init/reset/var+HD/writeback combined; check-node specialized
// for kMaxChkDeg==2 (this code) to drop inner degree loops.

#ifndef RELAY_BP_KERNELS_HPP
#define RELAY_BP_KERNELS_HPP

#include "relay_bp_types.hpp"

#include <sycl/sycl.hpp>

#include <chrono>
#include <iostream>

class MinSumBPDecodeID;

// True if H has an edge between check c and variable v (CSR scan, unrolled).
inline bool has_edge(int c, int v) {
  bool edge = false;
#pragma unroll
  for (int i = 0; i < kMaxChkDeg; ++i) {
    if (i < kChkDeg[c] && kChkVars[c][i] == v) {
      edge = true;
    }
  }
  return edge;
}

// Full Relay-BP-SS (S=1): outer legs r=0..R-1, inner DMem-BP iterations.
template <typename MsgT = ::MsgT>
inline void min_sum_bp_decode(const DetectorWord &det_in,
                              const MsgT priors[kNVar], DecodeWord &out) {
  MsgT v2c[kNChk][kNVar];
  MsgT c2v[kNChk][kNVar];
  MsgT posterior[kNVar];
  MsgT posterior_prev[kNVar];
  const MsgT z = msg_zero(MsgT{});
  const MsgT alpha = msg_one(MsgT{});

  // Init (fused): zero messages, seed M_j(0)=λ_j and edge v2c←λ.
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    posterior[v] = z;
    posterior_prev[v] = priors[v];
  }
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
      c2v[c][v] = z;
      v2c[c][v] = has_edge(c, v) ? priors[v] : z;
    }
  }

  unsigned char decoding[kNVar] = {0, 0, 0};
  unsigned char decoded_det[kNChk] = {0, 0};
  unsigned char decoding_out[kNVar] = {0, 0, 0};
  unsigned char decoded_det_out[kNChk] = {0, 0};
  bool success = false;
  bool done = false;
  int total_iters = 0;

  // Flattened leg×t into one statically bounded `it` loop (no break);
  // write-back gated by latched `done` (handbook 5_2_1).
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
    const bool do_reset = (t == 0) && (leg > 0);

    // Leg-start reset (fused): one (c,v) pass — clear msgs / reseed v2c
    // from priors when do_reset; else identity (ternary, not conditional loop).
#pragma unroll
    for (int c = 0; c < kNChk; ++c) {
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        c2v[c][v] = do_reset ? z : c2v[c][v];
        v2c[c][v] =
            do_reset ? (has_edge(c, v) ? priors[v] : z) : v2c[c][v];
      }
    }

    // Check -> variable (Eq 1). For this code every check has degree 2
    // (kMaxChkDeg==2): gather / min-two / scatter fused into straight-line
    // ops inside the outer `c` loop (no inner degree for-loops).
#pragma unroll
    for (int c = 0; c < kNChk; ++c) {
      const int v0 = kChkVars[c][0];
      const int v1 = kChkVars[c][1];
      const MsgT m0 = v2c[c][v0];
      const MsgT m1 = v2c[c][v1];
      const bool acc_sign =
          (det_in.d[c] == 1) ^ msg_is_neg(m0) ^ msg_is_neg(m1);
      const MsgT a0 = msg_abs(m0);
      const MsgT a1 = msg_abs(m1);
      const MsgT min_m = (a0 < a1) ? a0 : a1;
      const MsgT second_m = (a0 < a1) ? a1 : a0;
      const MsgT out_min = alpha * min_m;
      const MsgT out_second = alpha * second_m;

      const bool s0 = acc_sign ^ msg_is_neg(m0);
      const bool s1 = acc_sign ^ msg_is_neg(m1);
      const MsgT mag0 = (a0 == min_m) ? out_second : out_min;
      const MsgT mag1 = (a1 == min_m) ? out_second : out_min;
      c2v[c][v0] = s0 ? static_cast<MsgT>(-mag0) : mag0;
      c2v[c][v1] = s1 ? static_cast<MsgT>(-mag1) : mag1;
    }

    // Variable -> check (Eq 2/3 + DMem-BP Eq 4) fused with M_j update and
    // hard decision (pseudocode line 9): one `v` loop.
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
      const int deg = kVarDeg[v];
      MsgT sum = msg_apply_gamma(priors[v], posterior_prev[v], leg, v);
#pragma unroll
      for (int i = 0; i < kMaxVarDeg; ++i) {
        if (i < deg) {
          sum += c2v[kVarChecks[v][i]][v];
        }
      }
      posterior[v] = sum;
      posterior_prev[v] = sum;  // M_j(t-1) <- M_j(t)
      decoding[v] = msg_is_nonpos(sum) ? 1 : 0;
#pragma unroll
      for (int i = 0; i < kMaxVarDeg; ++i) {
        if (i < deg) {
          const int c = kVarChecks[v][i];
          v2c[c][v] = sum - c2v[c][v];
        }
      }
    }

    // Syndrome check (line 10) + gated write-back of det side in one `c`
    // pass; decoding_out still needs its own `v` pass (different bound).
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
      decoded_det_out[c] = done ? decoded_det_out[c] : syn;
    }
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
      decoding_out[v] = done ? decoding_out[v] : decoding[v];
    }
    if (!done) {
      success = iter_success;
      ++total_iters;
      done = iter_success;
    }
  }

  // Final host-visible copy (fused over max(kNVar,kNChk)).
  constexpr int kCopyN = (kNVar > kNChk) ? kNVar : kNChk;
#pragma unroll
  for (int i = 0; i < kCopyN; ++i) {
    if (i < kNVar) {
      out.decoding[i] = decoding_out[i];
    }
    if (i < kNChk) {
      out.decoded_detectors[i] = decoded_det_out[i];
    }
  }
  out.success = success ? 1 : 0;
  out.iterations = static_cast<unsigned char>(total_iters);
}

template <typename MsgT = ::MsgT>
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
#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        local_priors[v] = prior_acc[v];
      }
      DecodeWord local_out;
      min_sum_bp_decode<MsgT>(det_acc[0], local_priors, local_out);
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
