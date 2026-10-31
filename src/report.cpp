#include "report.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

std::string fixed(double value, int precision) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << value;
    return os.str();
}

}  // namespace

Report build_report(const ReportInput& in) {
    // Table / average: each dimension is capped at 100 (ethics raw can exceed).
    auto clamp100 = [](double v) { return std::min(100.0, std::max(0.0, v)); };
    double score_sum = 0;
    for (const auto& item : in.scores) {
        score_sum += clamp100(item.second);
    }
    const double overall = in.scores.empty() ? 0.0 : score_sum / static_cast<double>(in.scores.size());

    double time_sum = 0;
    long long token_sum = 0;
    double ttft_sum = 0;
    for (const auto& item : in.perf) {
        time_sum += item.second.time_s;
        token_sum += item.second.tokens;
        ttft_sum += item.second.ttft_s;
    }
    const double avg_ttft = in.perf.empty() ? 0.0 : ttft_sum / static_cast<double>(in.perf.size());
    const double avg_tok = time_sum > 0.0 ? static_cast<double>(token_sum) / time_sum : 0.0;

    std::ostringstream md;
    md << "# 本地大模型十一维能力与性能评测报告 (v3.0 cpp)\n";
    md << "- **测试模型**: `" << in.model << "`\n";
    md << "- **接口地址**: `" << in.api_url << "`\n";
    md << "- **评测时间**: `" << in.timestamp << "`\n";
    md << "- **代码沙盒**: `C++17 (g++ -std=c++17)`，维度 3 分数不可与旧版 Python asyncio 沙盒直接对比\n";
    if (!in.ethics_summary.empty()) {
        md << "- **伦理拒答率**: " << in.ethics_summary << "\n";
    }
    if (in.env_error) {
        md << "- **环境错误 (ENV_ERROR)**: **是** — 下列项不应记成模型能力\n";
        for (const auto& d : in.env_error_details) {
            md << "  - `" << d << "`\n";
        }
    }
    md << "- **综合能力平均得分**: **" << fixed(overall, 1) << " / 100**\n";
    md << "\n";
    md << "## 一、 十一维能力成绩总览表\n";
    md << "\n";
    md << "| 评测维度 | 得分 (满分100) |\n";
    md << "| :--- | :---: |\n";
    for (const auto& item : in.scores) {
        md << "| **" << item.first << "** | **" << fixed(clamp100(item.second), 1) << "** |\n";
    }
    md << "\n";
    md << "## 二、 独立附属性能卡 (Performance & Hardware Sidecard)\n";
    md << "> *注：附属性能指标与硬件加速状态独立分栏，不计入大模型核心能力总分。*\n";
    md << "\n";
    md << "| 性能指标 | 测量结果 |\n";
    md << "| :--- | :--- |\n";
    md << "| **硬件加速后端** | `" << in.hw_backend << "` |\n";
    md << "| **平均首字延迟 (TTFT)** | `" << fixed(avg_ttft * 1000.0, 1) << " ms` |\n";
    md << "| **平均生成速率** | `" << fixed(avg_tok, 1) << " tok/s` |\n";
    md << "| **评测进程峰值物理内存 (RSS)** | `" << fixed(in.peak_rss_mb, 1) << " MB` |\n";
    md << "| **确定性稳定性方差 (@ Temp 0)** | `Variance = " << fixed(in.stability_variance, 2) << "` |\n";

    nlohmann::ordered_json perf = nlohmann::ordered_json::object();
    for (const auto& item : in.perf) {
        perf[item.first] = {
            {"time_s", item.second.time_s},
            {"tokens", item.second.tokens},
            {"ttft_s", item.second.ttft_s},
        };
    }
    nlohmann::ordered_json scores = nlohmann::ordered_json::object();
    for (const auto& item : in.scores) {
        scores[item.first] = clamp100(item.second);
    }
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    root["model"] = in.model;
    root["api_url"] = in.api_url;
    root["overall"] = overall;
    root["env_error"] = in.env_error;
    root["env_error_details"] = in.env_error_details;
    root["scores"] = std::move(scores);
    root["perf"] = std::move(perf);
    root["avg_tok_s"] = avg_tok;
    root["avg_ttft_s"] = avg_ttft;
    root["peak_rss_mb"] = in.peak_rss_mb;
    root["hw_backend"] = in.hw_backend;
    root["stability_variance"] = in.stability_variance;
    root["ethics_refusal"] = in.ethics_summary;
    root["dim3_sandbox"] = "cpp17";
    root["dim3_note"] = "Dimension 3 compiles and runs C++17 with g++ -std=c++17. Scores are not comparable to the Python asyncio sandbox in local_llm_benchmark v3.";

    Report report;
    report.markdown = md.str();
    report.json = root.dump(2);
    report.json.push_back('\n');
    report.overall = overall;
    report.avg_tok_s = avg_tok;
    report.avg_ttft_s = avg_ttft;
    return report;
}

std::string json_sidecar_path(const std::string& markdown_path) {
    const auto slash = markdown_path.find_last_of('/');
    const auto dot = markdown_path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return markdown_path.substr(0, dot) + ".json";
    }
    return markdown_path + ".json";
}

void write_report_files(const std::string& markdown_path, const Report& report) {
    const std::string json_path = json_sidecar_path(markdown_path);
    std::ofstream md(markdown_path, std::ios::binary);
    if (!md) {
        throw std::runtime_error("failed to write " + markdown_path);
    }
    md << report.markdown;
    if (!md) {
        throw std::runtime_error("failed to write " + markdown_path);
    }
    std::ofstream js(json_path, std::ios::binary);
    if (!js) {
        throw std::runtime_error("failed to write " + json_path);
    }
    js << report.json;
    if (!js) {
        throw std::runtime_error("failed to write " + json_path);
    }
}
