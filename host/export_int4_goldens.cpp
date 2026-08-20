// Host-only int4 oracle exporter. Compiles the same decode as the FPGA kernel
// (-DRELAY_BP_HOST_ORACLE) and writes committed golden JSON for bit-exact TB.
//
// Build/run via scripts/export_int4_goldens.sh (production + multileg).

#include "relay_bp_decode.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Case {
  const char *name;
  unsigned char detectors[kNChk];
};

// Same four syndromes as the float RelayDecoderF32 goldens / RUST_EXPECTED.
const Case kCases[4] = {
    {"no_error", {0, 0}},
    {"error_qubit_0", {1, 0}},
    {"error_qubit_1", {1, 1}},
    {"error_qubit_2", {0, 1}},
};

const double kErrorPrior = 0.003;

std::string u8_array_json(const unsigned char *a, int n) {
  std::string s = "[";
  for (int i = 0; i < n; ++i) {
    if (i) {
      s += ", ";
    }
    s += std::to_string(static_cast<int>(a[i]));
  }
  s += "]";
  return s;
}

std::string int_array_json(const int *a, int n) {
  std::string s = "[";
  for (int i = 0; i < n; ++i) {
    if (i) {
      s += ", ";
    }
    s += std::to_string(a[i]);
  }
  s += "]";
  return s;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string out_path =
      (argc > 1) ? argv[1]
                 : std::string("test/golden/repetition_code_relay_int4.json");

  const double llr = std::log((1.0 - kErrorPrior) / kErrorPrior);
  double log_priors[kNVar] = {llr, llr, llr};
  EdgeMsg priors[kNVar];
  for (int v = 0; v < kNVar; ++v) {
    priors[v] = quantize_prior(log_priors[v]);
  }

  int prior_mags[kNVar];
  for (int v = 0; v < kNVar; ++v) {
    prior_mags[v] = priors[v].mag;
  }

  int beta_flat[kNumLegs * kNVar];
  for (int leg = 0; leg < kNumLegs; ++leg) {
    for (int v = 0; v < kNVar; ++v) {
      beta_flat[leg * kNVar + v] = kLegBetaInt[leg][v];
    }
  }

  std::vector<std::string> vector_objs;
  std::vector<std::string> multileg_objs;
  bool any_leg_gt0 = false;

  for (const auto &c : kCases) {
    DetectorWord det{};
    for (int i = 0; i < kNChk; ++i) {
      det.d[i] = c.detectors[i];
    }
    DecodeWord out{};
    min_sum_bp_decode(det, priors, out);
    const int leg = leg_reached_from_iters(static_cast<int>(out.iterations));
    if (leg > 0) {
      any_leg_gt0 = true;
      multileg_objs.push_back(
          std::string("    {\n") + "      \"name\": \"" + c.name + "\",\n" +
          "      \"iterations\": " + std::to_string(int(out.iterations)) +
          ",\n" + "      \"leg_reached\": " + std::to_string(leg) + "\n" +
          "    }");
    }

    vector_objs.push_back(
        std::string("    {\n") + "      \"name\": \"" + c.name + "\",\n" +
        "      \"detectors\": " + u8_array_json(c.detectors, kNChk) + ",\n" +
        "      \"expected_decoding\": " +
        u8_array_json(out.decoding, kNVar) + ",\n" +
        "      \"expected_decoded_detectors\": " +
        u8_array_json(out.decoded_detectors, kNChk) + ",\n" +
        "      \"success\": " + (out.success ? "true" : "false") + ",\n" +
        "      \"iterations\": " + std::to_string(int(out.iterations)) +
        ",\n" + "      \"leg_reached\": " + std::to_string(leg) + ",\n" +
        "      \"pre_iter\": " + std::to_string(kPreIter) + ",\n" +
        "      \"set_max_iter\": " + std::to_string(kSetMaxIter) + "\n" +
        "    }");
  }

  std::ofstream out(out_path);
  if (!out) {
    std::cerr << "failed to write " << out_path << "\n";
    return EXIT_FAILURE;
  }

  out << "{\n"
      << "  \"code\": \"repetition_3\",\n"
      << "  \"source\": \"ahls host int4 oracle (relay_bp_decode.hpp; same "
         "arithmetic as MinSumBPDecodeID)\",\n"
      << "  \"cross_checked\": true,\n"
      << "  \"cross_check_oracle\": \"device/relay_bp_decode.hpp "
         "min_sum_bp_decode\",\n"
      << "  \"cross_check_note\": \"Committed int4.2.8 golden. TB requires "
         "device == host oracle == this JSON (decoding, detectors, success, "
         "iterations).\",\n"
      << "  \"numeric_type\": \"int4.2.8\",\n"
      << "  \"int4_config\": {\n"
      << "    \"msg_mag_bits\": " << kMsgMagBits << ",\n"
      << "    \"prior_scale_S\": " << kPriorScaleS << ",\n"
      << "    \"mem_scale_M\": " << kMemScaleM << ",\n"
      << "    \"post_sat\": " << kPostSat << ",\n"
      << "    \"alpha\": \"1-2^{-(t+1)} via shifts\",\n"
      << "    \"pre_iter\": " << kPreIter << ",\n"
      << "    \"set_max_iter\": " << kSetMaxIter << ",\n"
      << "    \"num_legs\": " << kNumLegs << ",\n"
      << "    \"prior_mags\": " << int_array_json(prior_mags, kNVar) << ",\n"
      << "    \"leg_beta_int_flat\": "
      << int_array_json(beta_flat, kNumLegs * kNVar) << "\n"
      << "  },\n"
      << "  \"error_priors\": [" << kErrorPrior << ", " << kErrorPrior << ", "
      << kErrorPrior << "],\n"
      << "  \"log_prior_ratios\": [" << log_priors[0] << ", " << log_priors[1]
      << ", " << log_priors[2] << "],\n"
      << "  \"multi_leg_exercised\": " << (any_leg_gt0 ? "true" : "false")
      << ",\n";

  if (!multileg_objs.empty()) {
    out << "  \"multileg_vectors\": [\n";
    for (size_t i = 0; i < multileg_objs.size(); ++i) {
      out << multileg_objs[i];
      if (i + 1 < multileg_objs.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "  ],\n";
  }

  out << "  \"vectors\": [\n";
  for (size_t i = 0; i < vector_objs.size(); ++i) {
    out << vector_objs[i];
    if (i + 1 < vector_objs.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n"
      << "}\n";

  std::cout << "Wrote " << out_path << " (pre_iter=" << kPreIter
            << ", multi_leg_exercised=" << (any_leg_gt0 ? "true" : "false")
            << ")\n";
  for (size_t i = 0; i < vector_objs.size(); ++i) {
    // Re-run for console summary
    DetectorWord det{};
    for (int j = 0; j < kNChk; ++j) {
      det.d[j] = kCases[i].detectors[j];
    }
    DecodeWord dw{};
    min_sum_bp_decode(det, priors, dw);
    std::cout << "  " << kCases[i].name << ": iters=" << int(dw.iterations)
              << " leg=" << leg_reached_from_iters(int(dw.iterations))
              << " decoding=[" << int(dw.decoding[0]) << ","
              << int(dw.decoding[1]) << "," << int(dw.decoding[2])
              << "] success=" << int(dw.success) << "\n";
  }
  return EXIT_SUCCESS;
}
