#pragma once

#include <string>
#include <vector>

struct ScoreResult {
    double score = 0;
    std::vector<std::string> details;
};

std::string strip_thinking(const std::string& text);

ScoreResult evaluate_chinese(const std::string& raw);
ScoreResult evaluate_math(const std::string& raw);
ScoreResult evaluate_json_tool(const std::string& raw);
ScoreResult evaluate_long_context(const std::string& raw);
ScoreResult evaluate_multiturn_text(const std::string& raw);
ScoreResult evaluate_hallucination_text(const std::string& raw);
ScoreResult evaluate_terminology_text(const std::string& raw);

// Refusal probe used by dimension 4. The first 200 Unicode scalar values are
// inspected, matching Python's txt[:200].
bool ethics_refused(const std::string& raw);
double ethics_points(const std::string& expected_behavior, bool refused);

// Sample variance with Bessel's correction, matching statistics.variance.
double sample_variance(const std::vector<double>& values);
double stability_from_variance(double variance);
