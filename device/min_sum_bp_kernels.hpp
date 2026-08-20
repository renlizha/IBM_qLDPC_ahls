// Min-Sum Relay-BP-SS decode kernel for the 3-variable / 2-check repetition
// code. Maps Algorithm 1 (arXiv:2506.01779v2 Appendix D) and
// rust/relay RelayDecoder::decode_detailed / decode_inner / init_next_set.
// v1: one single_task kernel; pipe-connected check/variable kernels deferred.

#ifndef MIN_SUM_BP_KERNELS_HPP
#define MIN_SUM_BP_KERNELS_HPP

#include "min_sum_bp_types.hpp"

#include <sycl/sycl.hpp>

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
  for (int c = 0; c < kNChk; ++c) {
    for (int v = 0; v < kNVar; ++v) {
      v2c[c][v] = 0.0f;
      c2v[c][v] = 0.0f;
    }
  }
  for (int v = 0; v < kNVar; ++v) {
    posterior[v] = 0.0f;
    posterior_prev[v] = priors[v];  // M_j(0) = lambda_j
    for (int i = 0; i < kVarDeg[v]; ++i) {
      v2c[kVarChecks[v][i]][v] = priors[v];
    }
  }

  unsigned char decoding[kNVar] = {0, 0, 0};
  unsigned char decoded_det[kNChk] = {0, 0};
  bool success = false;
  int total_iters = 0;
  const MsgT alpha = 1.0f;

  // Pseudocode lines 2-25 / RelayDecoder::decode_detailed outer loop.
  for (int leg = 0; leg < kNumLegs; ++leg) {
    if (leg > 0) {
      // Pseudocode line 3 for legs r>=1.
      // Rust: init_next_set (gamma via kLegGamma[leg]) +
      // initialize_check_to_variable / initialize_variable_to_check.
      // Do NOT reset posterior_prev — it carries M_j(t) from the previous
      // leg (pseudocode line 23 / the relay mechanism).
      for (int c = 0; c < kNChk; ++c) {
        for (int v = 0; v < kNVar; ++v) {
          v2c[c][v] = 0.0f;
          c2v[c][v] = 0.0f;
        }
      }
      for (int v = 0; v < kNVar; ++v) {
        for (int i = 0; i < kVarDeg[v]; ++i) {
          v2c[kVarChecks[v][i]][v] = priors[v];
        }
      }
    }

    const int max_iter_this_leg = (leg == 0) ? kPreIter : kSetMaxIter;

    // Pseudocode lines 4-20 / RelayDecoder::decode_inner.
    for (int t = 0; t < max_iter_this_leg; ++t) {
      // Check -> variable (Eq 1 / compute_check_to_variable).
      for (int c = 0; c < kNChk; ++c) {
        const int deg = kChkDeg[c];
        MsgT messages[kMaxChkDeg];
        bool acc_sign = (det_in.d[c] == 1);
        for (int i = 0; i < deg; ++i) {
          messages[i] = v2c[c][kChkVars[c][i]];
          acc_sign = acc_sign ^ (messages[i] < 0.0f);
        }

        MsgT min_m = fabsf(messages[0]);
        MsgT second_m = kMaxDataValue;
        for (int i = 1; i < deg; ++i) {
          const MsgT a = fabsf(messages[i]);
          if (a < min_m) {
            second_m = min_m;
            min_m = a;
          } else if (a < second_m) {
            second_m = a;
          }
        }

        const MsgT out_min = alpha * min_m;
        const MsgT out_second = alpha * second_m;

        for (int i = 0; i < deg; ++i) {
          const int v = kChkVars[c][i];
          const bool sign = acc_sign ^ (messages[i] < 0.0f);
          const MsgT mag =
              (fabsf(messages[i]) == min_m) ? out_second : out_min;
          c2v[c][v] = sign ? -mag : mag;
        }
      }

      // Variable -> check (Eq 2/3) with DMem-BP bias (Eq 4 / line 5):
      //   Lambda_j(t) = (1-gamma_j(r))*lambda_j + gamma_j(r)*M_j(t-1)
      for (int v = 0; v < kNVar; ++v) {
        const int deg = kVarDeg[v];
        const MsgT gamma_v = kLegGamma[leg][v];
        const MsgT lambda_j =
            clip_msg(priors[v] * (1.0f - gamma_v) + posterior_prev[v] * gamma_v);
        MsgT running = lambda_j;
        for (int i = 0; i < deg; ++i) {
          const int c = kVarChecks[v][i];
          v2c[c][v] = running;
          running += c2v[c][v];
        }
        posterior[v] = running;

        running = 0.0f;
        for (int i = deg - 1; i >= 0; --i) {
          const int c = kVarChecks[v][i];
          v2c[c][v] += running;
          running += c2v[c][v];
        }
      }

      for (int c = 0; c < kNChk; ++c) {
        for (int v = 0; v < kNVar; ++v) {
          v2c[c][v] = clip_msg(v2c[c][v]);
        }
      }
      for (int v = 0; v < kNVar; ++v) {
        posterior[v] = clip_msg(posterior[v]);
        posterior_prev[v] = posterior[v];  // M_j(t-1) <- M_j(t)
      }

      // Pseudocode line 9 (HD) + line 10 (H*e == sigma).
      for (int v = 0; v < kNVar; ++v) {
        decoding[v] = (posterior[v] <= 0.0f) ? 1 : 0;
      }

      success = true;
      for (int c = 0; c < kNChk; ++c) {
        unsigned char syn = 0;
        for (int i = 0; i < kChkDeg[c]; ++i) {
          syn ^= decoding[kChkVars[c][i]];
        }
        decoded_det[c] = syn;
        if (syn != det_in.d[c]) {
          success = false;
        }
      }

      ++total_iters;
      if (success) {
        // Pseudocode line 18. Weight-comparison / keep-lowest-weight
        // (lines 12,14-17) omitted: deliberate S=1 simplification (Rust
        // StoppingCriterion::NConv{stop_after:1}); deferred if S>1 needed.
        break;
      }
    }

    // Pseudocode lines 21-22 (s == S); S=kStopNConv=1.
    if (success) {
      break;
    }
    // else: fall through. posterior_prev already holds this leg's final
    // M_j(t) (pseudocode line 23 is a no-op — never separately reset).
  }

  for (int v = 0; v < kNVar; ++v) {
    out.decoding[v] = decoding[v];
  }
  for (int c = 0; c < kNChk; ++c) {
    out.decoded_detectors[c] = decoded_det[c];
  }
  out.success = success ? 1 : 0;
  out.iterations = static_cast<unsigned char>(total_iters);
}

inline void launch_min_sum_bp(sycl::queue &q, const DetectorWord &det_in,
                              const MsgT priors[kNVar], DecodeWord &out) {
  sycl::buffer<DetectorWord, 1> det_buf{&det_in, sycl::range(1)};
  sycl::buffer<MsgT, 1> prior_buf{priors, sycl::range(kNVar)};
  sycl::buffer<DecodeWord, 1> out_buf{&out, sycl::range(1)};

  q.submit([&](sycl::handler &h) {
    sycl::accessor det_acc{det_buf, h, sycl::read_only};
    sycl::accessor prior_acc{prior_buf, h, sycl::read_only};
    sycl::accessor out_acc{out_buf, h, sycl::write_only, sycl::no_init};
    h.single_task<MinSumBPDecodeID>([=]() {
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
}

#endif
