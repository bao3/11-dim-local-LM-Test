#pragma once

#include <string>
#include <utility>
#include <vector>

struct PerfSample {
    double time_s = 0;
    long long tokens = 0;
    double ttft_s = 0;
};

struct ReportInput {
    std::string model;
    std::string api_url;
    std::string timestamp;
    std::string ethics_summary;
    std::vector<std::pair<std::string, double>> scores;
    std::vector<std::pair<std::string, PerfSample>> perf;
    double stability_variance = 0;
    double peak_rss_mb = 0;
    std::string hw_backend;
};

struct Report {
    std::string markdown;
    std::string json;
    double overall = 0;
    double avg_tok_s = 0;
    double avg_ttft_s = 0;
};

Report build_report(const ReportInput& in);
std::string json_sidecar_path(const std::string& markdown_path);
void write_report_files(const std::string& markdown_path, const Report& report);
