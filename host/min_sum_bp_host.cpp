// SYCL host: queue setup, buffers, kernel launch, result readback.

#include "min_sum_bp_host.hpp"
#include "min_sum_bp_kernels.hpp"

#include <sycl/ext/altera/fpga_extensions.hpp>

sycl::queue make_fpga_queue() {
#if FPGA_SIMULATOR
  auto selector = sycl::ext::altera::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
  auto selector = sycl::ext::altera::fpga_selector_v;
#else
  auto selector = sycl::ext::altera::fpga_emulator_selector_v;
#endif
  return sycl::queue(selector);
}

std::string device_name(sycl::queue &q) {
  return q.get_device().get_info<sycl::info::device::name>();
}

void load_priors(const double log_prior_ratios[kNVar], MsgT priors[kNVar]) {
  for (int v = 0; v < kNVar; ++v) {
    priors[v] = clip_msg(static_cast<MsgT>(log_prior_ratios[v]));
  }
}

DecodeWord decode_repetition(sycl::queue &q, const unsigned char detectors[kNChk],
                             const MsgT priors[kNVar]) {
  DetectorWord det{};
  for (int c = 0; c < kNChk; ++c) {
    det.d[c] = detectors[c];
  }
  DecodeWord out{};
  launch_min_sum_bp(q, det, priors, out);
  return out;
}
