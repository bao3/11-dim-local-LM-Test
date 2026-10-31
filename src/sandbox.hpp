#pragma once

#include "scoring.hpp"

#include <string>

// Longest fenced block. A language tag on the opening fence line is not part of the code.
std::string extract_longest_fence(const std::string& text);

// Removes function definitions named main so the hidden harness can supply its own.
std::string strip_main_functions(const std::string& code);

// Compile with g++ -std=c++17 and run the aging/order harness.
// +20 when the snippet compiles, +80 when the sandbox order is T1, T2, T3.
ScoreResult evaluate_code(const std::string& raw);
