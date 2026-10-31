#include "http.hpp"
#include "long_context.hpp"
#include "perf.hpp"
#include "prompt_data.hpp"
#include "prompts.hpp"
#include "report.hpp"
#include "sandbox.hpp"
#include "scoring.hpp"
#include "selftest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string api_url = "http://127.0.0.1:8081/v1/chat/completions";
    std::string api_key = "sk-local-test";
    std::string model;
    std::string output = "benchmark_report_v3.md";
    int max_tokens = 65536;
    int timeout_s = 2400;
    bool self_test = false;
    bool help = false;
};

void print_help() {
    std::cout
        << "ps5ai-11dim — PS5AI eleven-dimension local LLM benchmark (C++17)\n"
        << "\n"
        << "Usage:\n"
        << "  ps5ai-11dim --model <name> [options]\n"
        << "  ps5ai-11dim --self-test\n"
        << "  ps5ai-11dim --help\n"
        << "\n"
        << "Options:\n"
        << "  --api-url <url>     OpenAI-compatible chat completions endpoint\n"
        << "                      (default: http://127.0.0.1:8081/v1/chat/completions)\n"
        << "  --api-key <key>     Bearer token (default: sk-local-test, or env PS5AI_API_KEY)\n"
        << "  --model <name>      Model id sent in the request body (required for a live run)\n"
        << "  --output <path>     Markdown report path (default: benchmark_report_v3.md)\n"
        << "                      A JSON sidecar is written next to it.\n"
        << "  --max-tokens <n>    max_tokens sent to the API (default: 65536)\n"
        << "  --timeout <sec>     Per-request timeout (default: 2400)\n"
        << "  --self-test         Score math/Chinese/JSON/sandbox offline. No LLM.\n"
        << "  --help              Show this help\n"
        << "\n"
        << "Dimension 3 compiles model output with g++ -std=c++17 and runs it.\n"
        << "Those scores are not comparable to the old Python asyncio sandbox.\n"
        << "Thinking is disabled (enable_thinking=false, chat_template_kwargs.enable_thinking=false).\n";
}

std::string need_value(int& i, int argc, char** argv, const std::string& flag) {
    if (i + 1 >= argc) {
        throw UsageError("missing value for " + flag);
    }
    ++i;
    return argv[i];
}

int to_positive(const std::string& text, const std::string& flag) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size() || value <= 0) {
            throw UsageError(flag + " must be a positive integer");
        }
        return value;
    } catch (const UsageError&) {
        throw;
    } catch (const std::exception&) {
        throw UsageError(flag + " must be a positive integer");
    }
}

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](const std::string& flag) {
            const std::string prefix = flag + "=";
            if (arg == flag) {
                return need_value(i, argc, argv, flag);
            }
            if (arg.compare(0, prefix.size(), prefix) == 0) {
                return arg.substr(prefix.size());
            }
            return std::string();
        };
        if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else if (arg == "--self-test") {
            opt.self_test = true;
        } else if (arg.rfind("--api-url", 0) == 0) {
            const std::string value = take("--api-url");
            if (value.empty() && arg != "--api-url") {
                throw UsageError("unknown argument: " + arg);
            }
            opt.api_url = value;
        } else if (arg.rfind("--api-key", 0) == 0) {
            const std::string value = take("--api-key");
            if (value.empty() && arg != "--api-key") {
                throw UsageError("unknown argument: " + arg);
            }
            opt.api_key = value;
        } else if (arg.rfind("--model", 0) == 0) {
            const std::string value = take("--model");
            if (value.empty() && arg != "--model") {
                throw UsageError("unknown argument: " + arg);
            }
            opt.model = value;
        } else if (arg.rfind("--output", 0) == 0) {
            const std::string value = take("--output");
            if (value.empty() && arg != "--output") {
                throw UsageError("unknown argument: " + arg);
            }
            opt.output = value;
        } else if (arg.rfind("--max-tokens", 0) == 0) {
            opt.max_tokens = to_positive(take("--max-tokens"), "--max-tokens");
        } else if (arg.rfind("--timeout", 0) == 0) {
            opt.timeout_s = to_positive(take("--timeout"), "--timeout");
        } else {
            throw UsageError("unknown argument: " + arg);
        }
    }
    return opt;
}

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm local {};
    localtime_r(&t, &local);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return {};
    }
    return buf;
}

void print_details(const ScoreResult& result) {
    for (const auto& line : result.details) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "  score=" << std::fixed << std::setprecision(1) << result.score << std::endl;
}

struct Agg {
    double time_s = 0;
    long long tokens = 0;
    double ttft_sum = 0;
    int n = 0;
    std::chrono::steady_clock::time_point wall_start{};
    bool wall = false;

    void begin() {
        wall_start = std::chrono::steady_clock::now();
        wall = true;
    }
    void add(const LlmResponse& response) {
        time_s += response.elapsed_s;
        tokens += response.tokens;
        ttft_sum += response.ttft_s;
        ++n;
    }
    PerfSample sample() const {
        PerfSample perf;
        if (wall) {
            perf.time_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        } else {
            perf.time_s = time_s;
        }
        perf.tokens = tokens;
        perf.ttft_s = n > 0 ? ttft_sum / n : 0;
        return perf;
    }
};

int run_benchmark(const Options& opt) {
    LlmClient client;
    client.api_url = opt.api_url;
    client.api_key = opt.api_key;
    client.model = opt.model;
    client.max_tokens = opt.max_tokens;
    client.timeout_s = opt.timeout_s;

    std::cout << std::string(78, '=') << "\n";
    std::cout << "PS5AI 十一维评测 | 模型: " << opt.model << "\n";
    std::cout << "维度 3 沙盒: g++ -std=c++17 （分数不可与旧 Python asyncio 沙盒对比）\n";
    std::cout << "维度 10 与维度 4 共用同一次伦理光谱请求结果（不重复打接口）\n";
    std::cout << std::string(78, '=') << std::endl;

    std::vector<std::pair<std::string, double>> scores;
    std::vector<std::pair<std::string, PerfSample>> perf;
    auto single = [&](const std::string& prompt, double temperature) {
        return client.complete({{"user", prompt}}, temperature);
    };
    bool had_env_error = false;
    std::vector<std::string> env_error_details;
    auto note_env = [&](const std::string& detail) {
        had_env_error = true;
        env_error_details.push_back(detail);
    };
    auto clamp100 = [](double v) { return std::min(100.0, std::max(0.0, v)); };
    auto fail_dim = [&](const std::string& name, const std::string& dim_key, const std::string& err) {
        std::cout << "  [ERROR] " << err << "\n";
        std::cout << "  score=0.0 (维度失败，继续后续维度)\n";
        scores.emplace_back(name, 0.0);
        perf.emplace_back(dim_key, PerfSample{});
    };

    double ethics_score_raw = 0;
    double ethics_score = 0;
    std::string ethics_summary_str;

    // 1 Chinese
    std::cout << "[1/11] 正在执行：中文理解测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptChinese(), 0.2);
        ScoreResult sc = evaluate_chinese(r.content);
        print_details(sc);
        scores.emplace_back("1. 中文语境理解", clamp100(sc.score));
        perf.emplace_back("1", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("1. 中文语境理解", "1", ex.what());
    }

    // 2 Math
    std::cout << "[2/11] 正在执行：7步数学推理测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptMath(), 0.0);
        ScoreResult sc = evaluate_math(r.content);
        print_details(sc);
        scores.emplace_back("2. 7步数学推理", clamp100(sc.score));
        perf.emplace_back("2", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("2. 7步数学推理", "2", ex.what());
    }

    // 3 Code sandbox
    std::cout << "[3/11] 正在执行：C++17 代码沙盒实测..." << std::endl;
    try {
        LlmResponse r = single(prompt_code(), 0.1);
        ScoreResult sc = evaluate_code(r.content);
        print_details(sc);
        for (const auto& line : sc.details) {
            if (line.find("[ENV_ERROR]") != std::string::npos) {
                std::cout << "  !! 环境问题（非模型能力）：见上 details\n";
                note_env(line);
            }
        }
        scores.emplace_back("3. 异步并发代码实测", clamp100(sc.score));
        perf.emplace_back("3", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("3. 异步并发代码实测", "3", ex.what());
    }

    // 4 Ethics
    std::cout << "[4/11] 正在执行：伦理光谱与被拒率测试..." << std::endl;
    try {
        Agg ethics;
        ethics.begin();
        ethics_score_raw = 0;
        int refused = 0;
        for (int i = 0; i < kEthicsCaseCount; ++i) {
            const EthicsCase& item = kEthicsCases[i];
            LlmResponse er = single(item.prompt, 0.2);
            ethics.add(er);
            const bool is_refused = ethics_refused(er.content);
            if (is_refused) {
                ++refused;
            }
            const double points = ethics_points(item.expected, is_refused);
            ethics_score_raw += points;
            std::cout << "  " << item.id << " " << item.name << " expected=" << item.expected
                      << (is_refused ? " refused" : " answered") << " +" << points << "\n";
        }
        const PerfSample ethics_perf = ethics.sample();
        std::ostringstream ethics_summary;
        ethics_summary << std::fixed << std::setprecision(1);
        ethics_summary << "总拒答率: " << refused << "/" << kEthicsCaseCount << " ("
                       << (100.0 * refused / kEthicsCaseCount) << "%)";
        ethics_summary_str = ethics_summary.str();
        ethics_score = clamp100(ethics_score_raw);
        std::cout << "  " << ethics_summary_str << "\n";
        if (ethics_score_raw > 100.0) {
            std::cout << "  raw_score=" << std::fixed << std::setprecision(1) << ethics_score_raw
                      << " -> clamped=" << ethics_score << std::endl;
        } else {
            std::cout << "  score=" << std::fixed << std::setprecision(1) << ethics_score << std::endl;
        }
        scores.emplace_back("4. 伦理安全与边界", ethics_score);
        perf.emplace_back("4", ethics_perf);
    } catch (const std::exception& ex) {
        ethics_score = 0;
        ethics_score_raw = 0;
        ethics_summary_str = std::string("伦理维失败: ") + ex.what();
        fail_dim("4. 伦理安全与边界", "4", ex.what());
    }

    // 5 JSON
    std::cout << "[5/11] 正在执行：JSON 与 Tool Calling 测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptJsonTool(), 0.0);
        ScoreResult sc = evaluate_json_tool(r.content);
        print_details(sc);
        scores.emplace_back("5. JSON 与 Tool Calling", clamp100(sc.score));
        perf.emplace_back("5", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("5. JSON 与 Tool Calling", "5", ex.what());
    }

    // 6 Long context
    std::cout << "[6/11] 正在执行：长上下文大海捞针测试..." << std::endl;
    try {
        const std::string long_prompt = build_long_context_prompt(12000);
        LlmResponse r = single(long_prompt, 0.0);
        ScoreResult sc = evaluate_long_context(r.content);
        print_details(sc);
        scores.emplace_back("6. 长上下文抗干扰", clamp100(sc.score));
        perf.emplace_back("6", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("6. 长上下文抗干扰", "6", ex.what());
    }

    // 7 Multiturn
    std::cout << "[7/11] 正在执行：多轮指令随动与抗遗忘测试..." << std::endl;
    try {
        Agg multi;
        multi.begin();
        std::vector<ChatMessage> messages{{"user", kPromptMultiturn1()}};
        LlmResponse first = client.complete(messages, 0.0);
        multi.add(first);
        messages.push_back({"assistant", first.content});
        messages.push_back({"user", kPromptMultiturn2()});
        LlmResponse second = client.complete(messages, 0.0);
        multi.add(second);
        ScoreResult sc = evaluate_multiturn_text(second.content);
        print_details(sc);
        scores.emplace_back("7. 多轮指令随动抗遗忘", clamp100(sc.score));
        perf.emplace_back("7", multi.sample());
    } catch (const std::exception& ex) {
        fail_dim("7. 多轮指令随动抗遗忘", "7", ex.what());
    }

    // 8 Hallucination
    std::cout << "[8/11] 正在执行：幻觉与事实锚定防御测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptHallucination(), 0.0);
        ScoreResult sc = evaluate_hallucination_text(r.content);
        print_details(sc);
        scores.emplace_back("8. 幻觉与事实锚定", clamp100(sc.score));
        perf.emplace_back("8", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("8. 幻觉与事实锚定", "8", ex.what());
    }

    // 9 Terminology
    std::cout << "[9/11] 正在执行：中英混排与专业术语保真测试..." << std::endl;
    try {
        LlmResponse r = single(kPromptTerminology(), 0.0);
        ScoreResult sc = evaluate_terminology_text(r.content);
        print_details(sc);
        scores.emplace_back("9. 中英混排术语保真", clamp100(sc.score));
        perf.emplace_back("9", PerfSample{r.elapsed_s, r.tokens, r.ttft_s});
    } catch (const std::exception& ex) {
        fail_dim("9. 中英混排术语保真", "9", ex.what());
    }

    // 10 shares dim 4
    std::cout << "[10/11] 细粒度渗透灰区边界：沿用维度 4 的伦理光谱得分（含 E6 授权扫描；不重复请求）" << std::endl;
    scores.emplace_back("10. 细粒度渗透灰区边界", ethics_score);
    std::cout << "  score=" << std::fixed << std::setprecision(1) << ethics_score
              << " (与维度 4 同次结果" << (ethics_score_raw > 100.0 ? "，已封顶 100" : "") << ")" << std::endl;

    // 11 Stability
    std::cout << "[11/11] 正在执行：确定性稳定性方差测试 (@ Temp 0, 3次)..." << std::endl;
    double variance = 0;
    try {
        Agg stable;
        stable.begin();
        std::vector<double> math_scores;
        for (int i = 0; i < 3; ++i) {
            LlmResponse mr = single(kPromptMath(), 0.0);
            stable.add(mr);
            const double math_score = evaluate_math(mr.content).score;
            math_scores.push_back(math_score);
            std::cout << "  run " << (i + 1) << " math=" << std::fixed << std::setprecision(1) << math_score << "\n";
        }
        variance = sample_variance(math_scores);
        const double stability = clamp100(stability_from_variance(variance));
        std::cout << "  3次运行得分记录: [" << math_scores[0] << ", " << math_scores[1] << ", " << math_scores[2] << "]\n";
        std::cout << "  分数方差 (Variance): " << std::fixed << std::setprecision(2) << variance << "\n";
        std::cout << "  score=" << std::fixed << std::setprecision(1) << stability << std::endl;
        scores.emplace_back("11. 确定性稳定性方差", stability);
        perf.emplace_back("11", stable.sample());
    } catch (const std::exception& ex) {
        fail_dim("11. 确定性稳定性方差", "11", ex.what());
    }

    ReportInput input;
    input.model = opt.model;
    input.api_url = opt.api_url;
    input.timestamp = now_stamp();
    input.ethics_summary = ethics_summary_str;
    input.scores = scores;
    input.perf = perf;
    input.stability_variance = variance;
    input.peak_rss_mb = peak_rss_mb();
    input.hw_backend = detect_hardware_acceleration();
    input.env_error = had_env_error;
    input.env_error_details = env_error_details;
    const Report report = build_report(input);
    write_report_files(opt.output, report);

    std::cout << "\n十一维评测完成。综合得分: " << std::fixed << std::setprecision(1) << report.overall << "/100\n";
    std::cout << "Markdown: " << opt.output << "\n";
    std::cout << "JSON: " << json_sidecar_path(opt.output) << std::endl;
    if (had_env_error) {
        std::cerr << "ENV_ERROR: 评测环境异常，退出码 3（报告已写出，勿把相关维当成模型能力）\n";
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        // Prefer explicit --api-key; otherwise allow PS5AI_API_KEY (avoids putting secrets only on argv).
        bool api_key_on_argv = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--api-key" || a.rfind("--api-key=", 0) == 0) {
                api_key_on_argv = true;
                break;
            }
        }
        if (!api_key_on_argv) {
            if (const char* env = std::getenv("PS5AI_API_KEY")) {
                if (env[0] != '\0') {
                    opt.api_key = env;
                }
            }
        }
        if (opt.help) {
            print_help();
            return 0;
        }
        if (opt.self_test) {
            return run_self_test();
        }
        if (opt.model.empty()) {
            std::cerr << "error: --model is required\n\n";
            print_help();
            return 2;
        }
        return run_benchmark(opt);
    } catch (const UsageError& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        print_help();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
