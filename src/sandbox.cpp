#include "sandbox.hpp"

#include "paths.hpp"
#include "utf8.hpp"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string error;
};

struct RunOptions {
    int timeout_s = 30;
    std::string cwd;
    bool limit_resources = false;
};

std::string one_line(std::string text, std::size_t limit) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

ProcResult run_process(const std::vector<std::string>& args, const RunOptions& opt) {
    ProcResult result;
    if (args.empty()) {
        result.error = "empty argv";
        return result;
    }
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        result.error = "pipe failed";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return result;
    }
    if (pid == 0) {
        // Own process group so the parent can SIGKILL the whole tree (g++ → cc1plus, etc.).
        if (setpgid(0, 0) != 0) {
            setsid();
        }
        if (!opt.cwd.empty() && chdir(opt.cwd.c_str()) != 0) {
            _exit(126);
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        if (opt.limit_resources) {
            struct rlimit rl {};
            rl.rlim_cur = 8;
            rl.rlim_max = 8;
            setrlimit(RLIMIT_CPU, &rl);
            rl.rlim_cur = 16ull << 20;
            rl.rlim_max = rl.rlim_cur;
            setrlimit(RLIMIT_FSIZE, &rl);
            rl.rlim_cur = 256ull << 20;
            rl.rlim_max = rl.rlim_cur;
            setrlimit(RLIMIT_AS, &rl);
            // RLIMIT_NPROC is UID-wide (threads included). Keep high so harness threads are not EAGAIN.
            rl.rlim_cur = 8192;
            rl.rlim_max = 8192;
            setrlimit(RLIMIT_NPROC, &rl);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    // Parent also sets child's pgid in case of race before child runs setpgid.
    setpgid(pid, pid);
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(opt.timeout_s);
    bool out_open = true;
    bool err_open = true;
    bool reaped = false;
    int status = 0;
    constexpr std::size_t kMaxCapture = 2 * 1024 * 1024;

    auto read_fd = [](int fd, std::string& dest, bool& open_flag) {
        char buf[4096];
        while (true) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                dest.append(buf, buf + n);
            } else if (n == 0) {
                open_flag = false;
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                open_flag = false;
                break;
            }
        }
    };

    auto kill_group = [&]() {
        // Negative pid = process group. Fall back to the direct child if needed.
        if (kill(-pid, SIGKILL) != 0) {
            kill(pid, SIGKILL);
        }
    };

    while (out_open || err_open || !reaped) {
        if (!reaped) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno != EINTR)) {
                reaped = true;
            }
        }
        if (!reaped && std::chrono::steady_clock::now() > deadline) {
            kill_group();
            waitpid(pid, &status, 0);
            reaped = true;
            result.timed_out = true;
        }
        if (result.stdout_text.size() + result.stderr_text.size() > kMaxCapture && !reaped) {
            kill_group();
            waitpid(pid, &status, 0);
            reaped = true;
            result.error = "output exceeded 2MB";
        }
        pollfd fds[2];
        nfds_t nf = 0;
        if (out_open) {
            fds[nf].fd = out_pipe[0];
            fds[nf].events = POLLIN;
            ++nf;
        }
        if (err_open) {
            fds[nf].fd = err_pipe[0];
            fds[nf].events = POLLIN;
            ++nf;
        }
        if (nf > 0) {
            poll(fds, nf, 50);
        }
        if (out_open) {
            read_fd(out_pipe[0], result.stdout_text, out_open);
        }
        if (err_open) {
            read_fd(err_pipe[0], result.stderr_text, err_open);
        }
        if (reaped) {
            // Drain once more, then stop: grandchildren must not hang us on open pipes.
            if (out_open) {
                read_fd(out_pipe[0], result.stdout_text, out_open);
            }
            if (err_open) {
                read_fd(err_pipe[0], result.stderr_text, err_open);
            }
            break;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    if (result.timed_out) {
        result.exit_code = -1;
    } else if (!result.error.empty() && result.error.find("output exceeded") != std::string::npos) {
        result.exit_code = -1;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

const char* kPreamble = R"PRE(
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
)PRE";

const char* kHarness = R"HARNESS(
int main() {
    std::mutex order_mu;
    std::vector<std::string> order;
    std::mutex err_mu;
    std::vector<std::string> errors;
    auto record = [&](const char* name) {
        std::lock_guard<std::mutex> lk(order_mu);
        order.push_back(name);
    };
    auto note = [&](const std::string& message) {
        std::lock_guard<std::mutex> lk(err_mu);
        errors.push_back(message);
    };
    auto escape = [](std::string text) {
        std::string out;
        for (char c : text) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
                out.push_back(c);
            } else if (c == '\n' || c == '\r') {
                out.push_back(' ');
            } else {
                out.push_back(c);
            }
        }
        return out;
    };
    try {
        PriorityScheduler sched(10.0, 10.0, 1, 0.25);
        std::thread t1([&] {
            try {
                sched.submit([&] {
                    record("T1");
                    std::this_thread::sleep_for(std::chrono::milliseconds(350));
                }, 10.0, 1, -1.0);
            } catch (const std::exception& ex) {
                note(std::string("T1:") + ex.what());
            } catch (...) {
                note("T1:unknown");
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::thread t2([&] {
            try {
                sched.submit([&] {
                    record("T2");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }, 2.0, 10, -1.0);
            } catch (const std::exception& ex) {
                note(std::string("T2:") + ex.what());
            } catch (...) {
                note("T2:unknown");
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        std::thread t3([&] {
            try {
                sched.submit([&] {
                    record("T3");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }, 2.0, 2, -1.0);
            } catch (const std::exception& ex) {
                note(std::string("T3:") + ex.what());
            } catch (...) {
                note("T3:unknown");
            }
        });
        t1.join();
        t2.join();
        t3.join();
        sched.shutdown(true);
        const bool pass = errors.empty() && order.size() == 3 && order[0] == "T1" && order[1] == "T2" && order[2] == "T3";
        std::cout << "RES_JSON:{\"passed\":[";
        if (pass) {
            std::cout << "\"T1_Aging\"";
        }
        std::cout << "],\"failed\":[";
        if (!pass) {
            std::string why = "Order:";
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (i) why += ",";
                why += order[i];
            }
            for (const auto& err : errors) {
                why += ";";
                why += err;
            }
            std::cout << "\"" << escape(why) << "\"";
        }
        std::cout << "]}" << std::endl;
        return pass ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cout << "RES_JSON:{\"passed\":[],\"failed\":[\"" << escape(ex.what()) << "\"]}" << std::endl;
        return 1;
    }
}
)HARNESS";

bool ident_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}

void skip_string(const std::string& code, std::size_t& i, char quote) {
    ++i;
    while (i < code.size()) {
        if (code[i] == '\\') {
            i += 2;
            continue;
        }
        if (code[i] == quote) {
            ++i;
            return;
        }
        ++i;
    }
}

std::size_t match_brace(const std::string& code, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < code.size();) {
        const char c = code[i];
        if (c == '"' || c == '\'') {
            skip_string(code, i, c);
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            const auto nl = code.find('\n', i);
            i = nl == std::string::npos ? code.size() : nl + 1;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            const auto end = code.find("*/", i + 2);
            i = end == std::string::npos ? code.size() : end + 2;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
        ++i;
    }
    return std::string::npos;
}

std::size_t match_paren(const std::string& code, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < code.size();) {
        const char c = code[i];
        if (c == '"' || c == '\'') {
            skip_string(code, i, c);
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            const auto nl = code.find('\n', i);
            i = nl == std::string::npos ? code.size() : nl + 1;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            const auto end = code.find("*/", i + 2);
            i = end == std::string::npos ? code.size() : end + 2;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
        ++i;
    }
    return std::string::npos;
}

struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
};


std::string& last_cxx_storage() {
    static std::string value;
    return value;
}

bool is_executable(const std::string& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

bool compiler_accepts_version(const std::string& cxx) {
    RunOptions opt;
    opt.timeout_s = 5;
    const ProcResult ver = run_process({cxx, "--version"}, opt);
    return ver.exit_code == 0;
}

// Absolute paths must be executable. A bare name is accepted when `--version` runs.
bool take_env_compiler(const char* key, std::string& out) {
    const char* env = std::getenv(key);
    if (env == nullptr || env[0] == '\0') {
        return false;
    }
    const std::string path(env);
    if (path.find('/') != std::string::npos) {
        if (!is_executable(path)) {
            return false;
        }
        out = path;
        return true;
    }
    if (!compiler_accepts_version(path)) {
        return false;
    }
    out = path;
    return true;
}

std::string remember_cxx(std::string path) {
    last_cxx_storage() = path;
    return last_cxx_storage();
}

// /usr/bin/<name> first, then a PATH lookup via `--version` (execvp).
std::string pick_named_compiler(const char* name) {
    const std::string usr = std::string("/usr/bin/") + name;
    if (is_executable(usr)) {
        return usr;
    }
    if (compiler_accepts_version(name)) {
        return name;
    }
    return {};
}

std::string resolve_cxx_impl() {
    std::string picked;
    if (take_env_compiler("PS5AI_CXX", picked)) {
        return remember_cxx(std::move(picked));
    }
    if (take_env_compiler("PS5AI_GXX", picked)) {
        return remember_cxx(std::move(picked));
    }
    if (const char* root = std::getenv("PS5AI_TOOLCHAIN")) {
        if (root[0] != '\0') {
            const std::string cand = std::string(root) + "/bin/g++";
            if (is_executable(cand)) {
                return remember_cxx(cand);
            }
        }
    }
    const std::string exe_dir = executable_dir();
    if (!exe_dir.empty()) {
        const std::string beside = exe_dir + "/toolchain/bin/g++";
        if (is_executable(beside)) {
            return remember_cxx(beside);
        }
        const std::string parent = exe_dir + "/../toolchain/bin/g++";
        if (is_executable(parent)) {
            return remember_cxx(parent);
        }
    }
    for (const char* name : {"g++", "clang++", "c++"}) {
        const std::string found = pick_named_compiler(name);
        if (!found.empty()) {
            return remember_cxx(found);
        }
    }
    return remember_cxx("c++");
}

std::string cxx_version_line_impl(const std::string& cxx) {
    if (cxx.empty()) {
        return {};
    }
    RunOptions opt;
    opt.timeout_s = 5;
    const ProcResult ver = run_process({cxx, "--version"}, opt);
    if (ver.exit_code != 0) {
        return {};
    }
    const auto pos = ver.stdout_text.find('\n');
    std::string line = pos == std::string::npos ? ver.stdout_text : ver.stdout_text.substr(0, pos);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.size() > 200) {
        line.resize(200);
    }
    return line;
}

bool write_file_checked(const std::string& path, const std::string& content, std::string& err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "failed to open " + path;
        return false;
    }
    out << content;
    out.flush();
    if (!out) {
        err = "failed to write " + path;
        return false;
    }
    return true;
}

std::string make_temp_dir() {
    char tmpl[] = "/tmp/ps5ai-dim3-XXXXXX";
    if (mkdtemp(tmpl) == nullptr) {
        throw std::runtime_error("mkdtemp failed");
    }
    return tmpl;
}

}  // namespace

std::string resolve_cxx_compiler() {
    return resolve_cxx_impl();
}

std::string last_resolved_cxx() {
    if (last_cxx_storage().empty()) {
        return resolve_cxx_impl();
    }
    return last_cxx_storage();
}

std::string cxx_version_line(const std::string& cxx) {
    return cxx_version_line_impl(cxx);
}

std::string extract_longest_fence(const std::string& text) {
    const std::string fence = "```";
    std::string best;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto open = text.find(fence, i);
        if (open == std::string::npos) {
            break;
        }
        std::size_t content = open + fence.size();
        if (content < text.size() && text[content] != '\n') {
            const auto nl = text.find('\n', content);
            if (nl == std::string::npos) {
                break;
            }
            content = nl + 1;
        } else if (content < text.size() && text[content] == '\n') {
            ++content;
        }
        const auto close = text.find(fence, content);
        if (close == std::string::npos) {
            break;
        }
        std::string block = text.substr(content, close - content);
        if (block.size() > best.size()) {
            best = std::move(block);
        }
        i = close + fence.size();
    }
    return best;
}

std::string strip_main_functions(const std::string& code) {
    std::vector<Range> ranges;
    for (std::size_t i = 0; i < code.size();) {
        const char c = code[i];
        if (c == '"' || c == '\'') {
            skip_string(code, i, c);
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            const auto nl = code.find('\n', i);
            i = nl == std::string::npos ? code.size() : nl + 1;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            const auto end = code.find("*/", i + 2);
            i = end == std::string::npos ? code.size() : end + 2;
            continue;
        }
        if (c == 'R' && i + 1 < code.size() && code[i + 1] == '"') {
            std::size_t j = i + 2;
            std::string delim;
            while (j < code.size() && code[j] != '(' && delim.size() < 16) {
                delim.push_back(code[j]);
                ++j;
            }
            if (j < code.size() && code[j] == '(') {
                const std::string end = ")" + delim + "\"";
                const auto e = code.find(end, j + 1);
                if (e == std::string::npos) {
                    break;
                }
                i = e + end.size();
                continue;
            }
        }
        if (ident_char(c)) {
            const std::size_t start = i;
            ++i;
            while (i < code.size() && ident_char(code[i])) {
                ++i;
            }
            const bool boundary_before = start == 0 || !ident_char(code[start - 1]);
            if (boundary_before && code.compare(start, i - start, "main") == 0) {
                std::size_t k = i;
                while (k < code.size() && std::isspace(static_cast<unsigned char>(code[k]))) {
                    ++k;
                }
                if (k < code.size() && code[k] == '(') {
                    const auto paren_end = match_paren(code, k);
                    if (paren_end != std::string::npos) {
                        std::size_t b = paren_end + 1;
                        while (b < code.size() && std::isspace(static_cast<unsigned char>(code[b]))) {
                            ++b;
                        }
                        if (b < code.size() && code[b] == '{') {
                            const auto brace_end = match_brace(code, b);
                            if (brace_end != std::string::npos) {
                                std::size_t line_start = 0;
                                if (const auto nl = code.rfind('\n', start); nl != std::string::npos) {
                                    line_start = nl + 1;
                                }
                                std::size_t erase_from = line_start;
                                const std::string prefix = trim_copy(code.substr(line_start, start - line_start));
                                if (prefix.empty() && line_start > 0) {
                                    const std::size_t prev_end = line_start - 1;
                                    std::size_t prev_start = 0;
                                    if (prev_end > 0) {
                                        const auto nl2 = code.rfind('\n', prev_end - 1);
                                        prev_start = nl2 == std::string::npos ? 0 : nl2 + 1;
                                    }
                                    erase_from = prev_start;
                                }
                                ranges.push_back(Range{erase_from, brace_end + 1});
                                i = brace_end + 1;
                                continue;
                            }
                        }
                    }
                }
            }
            continue;
        }
        ++i;
    }
    if (ranges.empty()) {
        return code;
    }
    std::string out = code;
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        if (it->begin <= it->end && it->end <= out.size()) {
            out.erase(it->begin, it->end - it->begin);
        }
    }
    return out;
}

ScoreResult evaluate_code(const std::string& raw) {
    const std::string text = strip_thinking(raw);
    const std::string block = trim_copy(extract_longest_fence(text));
    if (block.empty()) {
        return {0.0, {"[+0] 未找到代码块"}};
    }
    ScoreResult result;
    try {
        const std::string dir = make_temp_dir();
        struct TempGuard {
            std::string path;
            ~TempGuard() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } guard{dir};
        const std::string gxx = resolve_cxx_compiler();
        const std::string syntax_path = dir + "/syntax.cpp";
        {
            std::string write_err;
            if (!write_file_checked(syntax_path, std::string(kPreamble) + "\n" + block + "\n", write_err)) {
                return {0.0, {"[ENV_ERROR][+0] 写沙盒文件失败: " + write_err}};
            }
        }
        RunOptions syntax_opt;
        syntax_opt.timeout_s = 30;
        syntax_opt.cwd = dir;
        const ProcResult syntax = run_process({gxx, "-std=c++17", "-fsyntax-only", "-pthread", syntax_path}, syntax_opt);
        if (syntax.exit_code == 127) {
            return {0.0, {"[ENV_ERROR][+0] 未找到 C++ 编译器（" + gxx + "），无法编译 C++17 沙盒"}};
        }
        if (!syntax.error.empty() && syntax.exit_code < 0 && !syntax.timed_out) {
            return {0.0, {"[ENV_ERROR][+0] 启动编译失败: " + syntax.error}};
        }
        if (syntax.timed_out) {
            return {0.0, {"[+0] 编译超时"}};
        }
        if (syntax.exit_code != 0) {
            const std::string why = one_line(syntax.stderr_text.empty() ? syntax.stdout_text : syntax.stderr_text, 400);
            return {0.0, {"[+0] 编译失败: " + why}};
        }
        result.score = 20.0;
        result.details.push_back("[+20] 编译通过");

        const std::string harness_path = dir + "/harness.cpp";
        const std::string binary = dir + "/sched_test";
        {
            std::string write_err;
            const std::string body = std::string(kPreamble) + "\n" + strip_main_functions(block) + "\n" + kHarness;
            if (!write_file_checked(harness_path, body, write_err)) {
                result.details.push_back("[ENV_ERROR][+0] 写沙盒 harness 失败: " + write_err);
                result.score = 0.0;
                return result;
            }
        }
        RunOptions link_opt;
        link_opt.timeout_s = 30;
        link_opt.cwd = dir;
        const ProcResult linked = run_process(
            {gxx, "-std=c++17", "-pthread", "-O0", "-o", binary, harness_path}, link_opt);
        if (linked.exit_code == 127) {
            result.details.push_back("[ENV_ERROR][+0] 未找到 C++ 编译器（" + gxx + "），链接失败");
            result.score = 0.0;
            return result;
        }
        if (linked.timed_out || linked.exit_code != 0) {
            const std::string why = one_line(linked.stderr_text.empty() ? linked.stdout_text : linked.stderr_text, 400);
            result.details.push_back(std::string("[+0] 沙盒链接失败: ") + (linked.timed_out ? "timeout " : "") + why);
            return result;
        }
        RunOptions run_opt;
        run_opt.timeout_s = 12;
        run_opt.cwd = dir;
        run_opt.limit_resources = true;
        const ProcResult ran = run_process({binary}, run_opt);
        if (ran.timed_out) {
            result.details.push_back("[+0] 运行超时");
            return result;
        }
        if (!ran.error.empty() && ran.error.find("output exceeded") != std::string::npos) {
            result.details.push_back("[+0] 运行输出过大: " + ran.error);
            return result;
        }
        const auto marker = ran.stdout_text.rfind("RES_JSON:");
        if (marker == std::string::npos) {
            const std::string why = one_line(ran.stderr_text, 200);
            result.details.push_back("[+0] 运行未能返回测试结果" + (why.empty() ? "" : " " + why));
            return result;
        }
        // Only trust RES_JSON when the harness process exited 0. Non-zero can forge passed.
        if (ran.exit_code != 0) {
            result.details.push_back("[+0] 沙盒进程非零退出 (exit=" + std::to_string(ran.exit_code) +
                                     ")，忽略可能伪造的 RES_JSON");
            return result;
        }
        const auto json_begin = ran.stdout_text.find('{', marker);
        if (json_begin == std::string::npos) {
            result.details.push_back("[+0] 运行未能返回测试结果");
            return result;
        }
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(ran.stdout_text.substr(json_begin));
        } catch (const std::exception&) {
            result.details.push_back("[+0] 运行未能返回测试结果");
            return result;
        }
        bool aging = false;
        if (parsed.contains("passed") && parsed["passed"].is_array()) {
            for (const auto& item : parsed["passed"]) {
                if (item.is_string() && item.get<std::string>() == "T1_Aging") {
                    aging = true;
                }
            }
        }
        if (aging) {
            result.score += 80.0;
            result.details.push_back("[+80] 优先级老化与调度顺序沙盒测试通过");
        } else {
            std::string failed = parsed.contains("failed") ? parsed["failed"].dump() : "[]";
            result.details.push_back("[+0] 沙盒测试未通过: " + one_line(failed, 300));
        }
    } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        if (msg.find("mkdtemp") != std::string::npos) {
            result.details.push_back(std::string("[ENV_ERROR][+0] ") + msg);
        } else {
            result.details.push_back(std::string("[+0] 运行异常: ") + msg);
        }
    }
    return result;
}
