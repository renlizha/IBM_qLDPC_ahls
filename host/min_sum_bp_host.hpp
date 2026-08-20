#ifndef MIN_SUM_BP_HOST_HPP
#define MIN_SUM_BP_HOST_HPP

#include "min_sum_bp_types.hpp"

#include <sycl/sycl.hpp>

#include <string>

sycl::queue make_fpga_queue();
std::string device_name(sycl::queue &q);
void load_priors(const double log_prior_ratios[kNVar], MsgT priors[kNVar]);
DecodeWord decode_repetition(sycl::queue &q, const unsigned char detectors[kNChk],
                             const MsgT priors[kNVar]);

#endif
