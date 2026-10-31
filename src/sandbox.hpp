#pragma once

#include "scoring.hpp"

#include <string>

// Longest fenced block. A language tag on the opening fence line is not part of the code.
std::string extract_longest_fence(const std::string& text);

// Removes function definitions named main so the hidden harness can supply its own.
std::string strip_main_functions(const std::string& code);

// Compile with -std=c++17 -pthread and run the aging/order harness.
// +20 when the snippet compiles, +80 when the sandbox order is T1, T2, T3.
ScoreResult evaluate_code(const std::string& raw);

// First usable C++ compiler:
// PS5AI_CXX → PS5AI_GXX → $PS5AI_TOOLCHAIN/bin/g++ →
// <exe>/toolchain/bin/g++ → <exe>/../toolchain/bin/g++ →
// /usr/bin/g++ → PATH g++ → clang++ → c++.
// Falls back to the name "c++" when nothing is proven executable.
std::string resolve_cxx_compiler();
std::string last_resolved_cxx();
// First line of `cxx --version`, truncated to 200 characters. Empty on failure.
std::string cxx_version_line(const std::string& cxx);
