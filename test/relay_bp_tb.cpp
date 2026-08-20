// Testbench: load ahls/test/golden/*.json and assert bit-exact decodings.

#include "relay_bp_host.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open " + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Tiny JSON helpers for this file's known schema (not a general parser).
std::string slice_after_key(const std::string &s, const std::string &key) {
  const std::string quoted = "\"" + key + "\"";
  auto pos = s.find(quoted);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON key: " + key);
  }
  pos = s.find(':', pos + quoted.size());
  if (pos == std::string::npos) {
    throw std::runtime_error("malformed JSON around key: " + key);
  }
  return s.substr(pos + 1);
}

std::vector<double> parse_number_array(const std::string &s, const std::string &key) {
  std::string rest = slice_after_key(s, key);
  auto l = rest.find('[');
  auto r = rest.find(']', l);
  if (l == std::string::npos || r == std::string::npos) {
    throw std::runtime_error("expected array for key: " + key);
  }
  std::string body = rest.substr(l + 1, r - l - 1);
  std::vector<double> out;
  std::stringstream ss(body);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.find_first_not_of(" \t\n\r") == std::string::npos) {
      continue;
    }
    out.push_back(std::stod(tok));
  }
  return out;
}

int parse_int_field(const std::string &obj, const std::string &key) {
  std::string rest = slice_after_key(obj, key);
  auto t = rest.find_first_not_of(" \t\n\r");
  if (t == std::string::npos) {
    throw std::runtime_error("expected int for key: " + key);
  }
  return std::stoi(rest.substr(t));
}

struct GoldenVector {
  std::string name;
  unsigned char detectors[kNChk];
  unsigned char expected_decoding[kNVar];
  unsigned char expected_decoded_detectors[kNChk];
  bool success;
  int iterations;
  bool has_iterations;
};

std::vector<std::string> split_objects(const std::string &array_body) {
  std::vector<std::string> objs;
  int depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < array_body.size(); ++i) {
    if (array_body[i] == '{') {
      if (depth == 0) {
        start = i;
      }
      ++depth;
    } else if (array_body[i] == '}') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        objs.push_back(array_body.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return objs;
}

std::string parse_string_field(const std::string &obj, const std::string &key) {
  std::string rest = slice_after_key(obj, key);
  auto q1 = rest.find('"');
  auto q2 = rest.find('"', q1 + 1);
  if (q1 == std::string::npos || q2 == std::string::npos) {
    throw std::runtime_error("expected string for key: " + key);
  }
  return rest.substr(q1 + 1, q2 - q1 - 1);
}

bool parse_bool_field(const std::string &obj, const std::string &key) {
  std::string rest = slice_after_key(obj, key);
  auto t = rest.find_first_not_of(" \t\n\r");
  if (t != std::string::npos && rest.compare(t, 4, "true") == 0) {
    return true;
  }
  return false;
}

void fill_u8_array(const std::vector<double> &vals, unsigned char *dst, int n,
                   const std::string &what) {
  if (static_cast<int>(vals.size()) != n) {
    throw std::runtime_error(what + " length mismatch");
  }
  for (int i = 0; i < n; ++i) {
    dst[i] = static_cast<unsigned char>(vals[i]);
  }
}

std::vector<GoldenVector> parse_vectors(const std::string &json) {
  std::string rest = slice_after_key(json, "vectors");
  auto l = rest.find('[');
  if (l == std::string::npos) {
    throw std::runtime_error("missing vectors array");
  }
  int depth = 0;
  std::size_t r = std::string::npos;
  for (std::size_t i = l; i < rest.size(); ++i) {
    if (rest[i] == '[') {
      ++depth;
    } else if (rest[i] == ']') {
      --depth;
      if (depth == 0) {
        r = i;
        break;
      }
    }
  }
  if (r == std::string::npos) {
    throw std::runtime_error("unterminated vectors array");
  }
  auto objs = split_objects(rest.substr(l + 1, r - l - 1));
  std::vector<GoldenVector> out;
  for (const auto &obj : objs) {
    GoldenVector v;
    v.name = parse_string_field(obj, "name");
    fill_u8_array(parse_number_array(obj, "detectors"), v.detectors, kNChk,
                  v.name + " detectors");
    fill_u8_array(parse_number_array(obj, "expected_decoding"), v.expected_decoding,
                  kNVar, v.name + " expected_decoding");
    fill_u8_array(parse_number_array(obj, "expected_decoded_detectors"),
                  v.expected_decoded_detectors, kNChk,
                  v.name + " expected_decoded_detectors");
    v.success = parse_bool_field(obj, "success");
    v.has_iterations = (obj.find("\"iterations\"") != std::string::npos);
    if (v.has_iterations) {
      v.iterations = parse_int_field(obj, "iterations");
    } else {
      v.iterations = 0;
    }
    out.push_back(v);
  }
  return out;
}

}  // namespace

bool run_golden_file(sycl::queue &q, const std::string &json_path) {
  const std::string json = read_file(json_path);
  auto log_priors_f = parse_number_array(json, "log_prior_ratios");
  if (static_cast<int>(log_priors_f.size()) != kNVar) {
    std::cerr << json_path << ": log_prior_ratios must have " << kNVar
              << " entries\n";
    return false;
  }
  auto vectors = parse_vectors(json);
  if (vectors.size() != 4) {
    std::cerr << json_path << ": expected 4 golden vectors, got "
              << vectors.size() << "\n";
    return false;
  }

  std::cout << "Golden file: " << json_path << std::endl;

  double log_priors[kNVar];
  for (int v = 0; v < kNVar; ++v) {
    log_priors[v] = log_priors_f[v];
  }
  MsgT priors[kNVar];
  load_priors(log_priors, priors);
  std::cout << "float32 log-priors:";
  for (int v = 0; v < kNVar; ++v) {
    std::cout << " " << priors[v];
  }
  std::cout << std::endl;

  int n_pass = 0;
  bool all_ok = true;
  for (const auto &vec : vectors) {
    DecodeWord got = decode_repetition(q, vec.detectors, priors);
    bool ok = (got.success != 0) == vec.success;
    for (int v = 0; v < kNVar; ++v) {
      ok = ok && (got.decoding[v] == vec.expected_decoding[v]);
    }
    for (int c = 0; c < kNChk; ++c) {
      ok = ok && (got.decoded_detectors[c] == vec.expected_decoded_detectors[c]);
    }
    if (vec.has_iterations) {
      ok = ok && (static_cast<int>(got.iterations) == vec.iterations);
    }

    std::cout << "  " << vec.name << ": detectors=[";
    for (int c = 0; c < kNChk; ++c) {
      std::cout << (c ? "," : "") << int(vec.detectors[c]);
    }
    std::cout << "] got decoding=[";
    for (int v = 0; v < kNVar; ++v) {
      std::cout << (v ? "," : "") << int(got.decoding[v]);
    }
    std::cout << "] expected=[";
    for (int v = 0; v < kNVar; ++v) {
      std::cout << (v ? "," : "") << int(vec.expected_decoding[v]);
    }
    std::cout << "] success=" << int(got.success)
              << " (expected " << int(vec.success) << ") iters="
              << int(got.iterations);
    if (vec.has_iterations) {
      std::cout << " (expected " << vec.iterations << ")";
    }
    std::cout << " -> " << (ok ? "PASS" : "FAIL") << std::endl;

    if (ok) {
      ++n_pass;
    } else {
      all_ok = false;
    }
  }

  std::cout << json_path << ": " << n_pass << "/" << vectors.size()
            << " golden vectors passed -> " << (all_ok ? "PASS" : "FAIL")
            << std::endl;
  return all_ok;
}

int main(int argc, char **argv) {
  std::vector<std::string> files;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      files.emplace_back(argv[i]);
    }
  } else {
    files.emplace_back("test/golden/repetition_code_relay.json");
  }

  try {
    sycl::queue q = make_fpga_queue();
    std::cout << "Running on device: " << device_name(q) << std::endl;

    bool all_ok = true;
    for (const auto &path : files) {
      const bool ok = run_golden_file(q, path);
      all_ok = all_ok && ok;
    }

    std::cout << (all_ok ? "PASSED" : "FAILED") << std::endl;
    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch (sycl::exception const &e) {
    std::cerr << "SYCL exception:\n" << e.what() << "\n";
    return EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
