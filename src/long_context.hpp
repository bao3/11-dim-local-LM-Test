#pragma once

#include <string>

// Port of build_long_context_prompt from live_local_llm_benchmark_v2.py.
// Lengths are Unicode code points, matching Python 3 len().
std::string build_long_context_prompt(int target_chars = 15000);
