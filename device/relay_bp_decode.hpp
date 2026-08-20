// Host- and device-callable int4 Relay-BP-SS decode (CNU/VNU flooding).
// This is the software int4 oracle: same arithmetic as the FPGA kernel body.
// Build host tools with -DRELAY_BP_HOST_ORACLE (no SYCL / no fpga_register).
//
// Maurer et al. (arXiv:2510.21600) adaptations in this file:
//   check_node_unit / variable_node_unit + bp_iteration_flood (Fig. 3 style)
// Still a software BP-iter loop (same model as ../rust/relay run_iteration) —
// NOT the paper FPGA's true 2-cycle CNU→VNU stage split.
// See docs/paper_fpga_adaptations.md.

#ifndef RELAY_BP_DECODE_HPP
#define RELAY_BP_DECODE_HPP

#include "relay_bp_types.hpp"

#ifndef RELAY_BP_HOST_ORACLE
#define RELAY_BP_FPGA_REGISTER [[intel::fpga_register]]
#else
#define RELAY_BP_FPGA_REGISTER
#endif

// ---- CNU (paper Fig. 3a): one check row ----
inline void check_node_unit(int c, const DetectorWord &det,
                            const EdgeMsg v2c[kNChk][kNVar],
                            EdgeMsg c2v[kNChk][kNVar], int t) {
  const int deg = kChkDeg[c];
  EdgeMsg messages[kMaxChkDeg];
  bool acc_sign = (det.d[c] == 1);

#pragma unroll
  for (int i = 0; i < kMaxChkDeg; ++i) {
    if (i < deg) {
      messages[i] = v2c[c][kChkVars[c][i]];
      acc_sign = acc_sign ^ messages[i].neg;
    }
  }

  std::uint8_t min_m = messages[0].mag;
  std::uint8_t second_m = kMagSentinel;
#pragma unroll
  for (int i = 1; i < kMaxChkDeg; ++i) {
    if (i < deg) {
      const std::uint8_t a = messages[i].mag;
      if (a < min_m) {
        second_m = min_m;
        min_m = a;
      } else if (a < second_m) {
        second_m = a;
      }
    }
  }

  const std::uint8_t out_min = apply_alpha(min_m, t);
  const std::uint8_t out_second = apply_alpha(second_m, t);

#pragma unroll
  for (int i = 0; i < kMaxChkDeg; ++i) {
    if (i < deg) {
      const int v = kChkVars[c][i];
      const bool sign = acc_sign ^ messages[i].neg;
      const std::uint8_t mag =
          (messages[i].mag == min_m) ? out_second : out_min;
      c2v[c][v] = EdgeMsg{sign, mag};
    }
  }
}

// ---- VNU + Relay adder (paper Fig. 3b/c): one variable column ----
// lambda_j is precomputed (parallel with CNU).
// Note: do NOT sprinkle fpga_reg on this loop-carried path — each register
// increases minimum II (experiments: heavy staging → II=17, light → II=13
// @480 MHz, both worse throughput than II=1 @ ~66 MHz).
inline void variable_node_unit(int v, PostT lambda_j,
                               const EdgeMsg c2v[kNChk][kNVar],
                               EdgeMsg v2c[kNChk][kNVar],
                               PostT posterior[kNVar]) {
  const int deg = kVarDeg[v];

  PostT running = lambda_j;
#pragma unroll
  for (int i = 0; i < kMaxVarDeg; ++i) {
    if (i < deg) {
      const int c = kVarChecks[v][i];
      v2c[c][v] = edge_from_int(static_cast<int>(running));
      running = sat_post(static_cast<int>(running) + edge_as_int(c2v[c][v]));
    }
  }
  posterior[v] = running;

  running = 0;
#pragma unroll
  for (int i = kMaxVarDeg - 1; i >= 0; --i) {
    if (i < deg) {
      const int c = kVarChecks[v][i];
      const int summed =
          edge_as_int(v2c[c][v]) + static_cast<int>(running);
      v2c[c][v] = edge_from_int(summed);
      running =
          sat_post(static_cast<int>(running) + edge_as_int(c2v[c][v]));
    }
  }
}

// Flooding: Λ mix ∥ CNU, then VNU.
inline void bp_iteration_flood(int leg, int t, const DetectorWord &det,
                               const EdgeMsg priors[kNVar],
                               EdgeMsg v2c[kNChk][kNVar],
                               EdgeMsg c2v[kNChk][kNVar], PostT posterior[kNVar],
                               PostT posterior_prev[kNVar]) {
  PostT lambda[kNVar];
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    lambda[v] = relay_mix(static_cast<PostT>(priors[v].mag), posterior_prev[v],
                          kLegBetaInt[leg][v]);
  }
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
    check_node_unit(c, det, v2c, c2v, t);
  }
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    variable_node_unit(v, lambda[v], c2v, v2c, posterior);
  }
}

inline void clear_messages(EdgeMsg v2c[kNChk][kNVar],
                           EdgeMsg c2v[kNChk][kNVar]) {
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
#pragma unroll
    for (int v = 0; v < kNVar; ++v) {
      v2c[c][v] = kEdgeZero;
      c2v[c][v] = kEdgeZero;
    }
  }
}

inline void init_v2c_from_priors(const EdgeMsg priors[kNVar],
                                EdgeMsg v2c[kNChk][kNVar]) {
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
#pragma unroll
    for (int i = 0; i < kMaxVarDeg; ++i) {
      if (i < kVarDeg[v]) {
        v2c[kVarChecks[v][i]][v] = priors[v];
      }
    }
  }
}

inline bool syndrome_matches(const DetectorWord &det,
                             const unsigned char decoding[kNVar],
                             unsigned char decoded_det[kNChk]) {
  bool success = true;
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
    unsigned char syn = 0;
#pragma unroll
    for (int i = 0; i < kMaxChkDeg; ++i) {
      if (i < kChkDeg[c]) {
        syn = static_cast<unsigned char>(syn ^ decoding[kChkVars[c][i]]);
      }
    }
    decoded_det[c] = syn;
    if (syn != det.d[c]) {
      success = false;
    }
  }
  return success;
}

// Map cumulative BP iterations → leg index that produced the final result.
inline int leg_reached_from_iters(int total_iters) {
  int rem = total_iters;
  for (int leg = 0; leg < kNumLegs; ++leg) {
    const int max_this = (leg == 0) ? kPreIter : kSetMaxIter;
    if (rem <= max_this) {
      return leg;
    }
    rem -= max_this;
  }
  return kNumLegs - 1;
}

// Full Relay-BP-SS (S=1) — software int4 oracle / kernel body.
inline void min_sum_bp_decode(const DetectorWord &det_in,
                              const EdgeMsg priors[kNVar], DecodeWord &out) {
  RELAY_BP_FPGA_REGISTER EdgeMsg v2c[kNChk][kNVar];
  RELAY_BP_FPGA_REGISTER EdgeMsg c2v[kNChk][kNVar];
  RELAY_BP_FPGA_REGISTER PostT posterior[kNVar];
  RELAY_BP_FPGA_REGISTER PostT posterior_prev[kNVar];

  clear_messages(v2c, c2v);
#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    posterior[v] = 0;
    posterior_prev[v] = static_cast<PostT>(priors[v].mag);
  }
  init_v2c_from_priors(priors, v2c);

  unsigned char decoding[kNVar] = {0, 0, 0};
  unsigned char decoded_det[kNChk] = {0, 0};
  bool success = false;
  int total_iters = 0;

  for (int leg = 0; leg < kNumLegs; ++leg) {
    if (leg > 0) {
      clear_messages(v2c, c2v);
      init_v2c_from_priors(priors, v2c);
    }

    const int max_iter_this_leg = (leg == 0) ? kPreIter : kSetMaxIter;

    for (int t = 0; t < max_iter_this_leg; ++t) {
      bp_iteration_flood(leg, t, det_in, priors, v2c, c2v, posterior,
                         posterior_prev);

#pragma unroll
      for (int v = 0; v < kNVar; ++v) {
        posterior_prev[v] = posterior[v];
        decoding[v] = (posterior[v] <= 0) ? 1 : 0;
      }

      success = syndrome_matches(det_in, decoding, decoded_det);
      ++total_iters;
      if (success) {
        break;
      }
    }

    if (success) {
      break;
    }
  }

#pragma unroll
  for (int v = 0; v < kNVar; ++v) {
    out.decoding[v] = decoding[v];
  }
#pragma unroll
  for (int c = 0; c < kNChk; ++c) {
    out.decoded_detectors[c] = decoded_det[c];
  }
  out.success = success ? 1 : 0;
  out.iterations = static_cast<unsigned char>(total_iters);
}

#endif
