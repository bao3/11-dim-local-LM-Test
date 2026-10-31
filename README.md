# 11-dim-local-LM-Test

这是我们自己在本地大模型上用的十一维能力评测工具（C++ 版）。

它不依赖 Python。一个可执行文件 `ps5ai-11dim` 去打 OpenAI 兼容的 `/v1/chat/completions`，跑完后写出 Markdown 成绩表和旁边的 JSON。我们用它给 PS5AI（索尼 PS5 上跑的本地 LLM 栈）做验收和日常冒烟，也用来对比不同 GGUF / 别名。

旧版是 Python harness。现在镜像里经常卸掉 Python，所以改成了纯 C++17。题目和打分尽量对齐旧版 v3；**只有第 3 维换了沙盒**：以前让模型写 Python `asyncio` 调度器并用系统 Python 跑；现在让模型写 C++17 `PriorityScheduler`，用本机 `g++ -std=c++17 -pthread` 编译再跑。所以第 3 维分数**不能**和旧 Python 版直接比。

---

## 我们实际怎么用

- **评测端（跑这个二进制）**：通常在局域网机器 `work`（`192.168.50.69`）上编译和执行。该机有 `g++`、`cmake`、`libcurl`。
- **被测端（模型服务）**：PS5AI 上的 `llama-server`，默认 OpenAI 兼容口是 `http://192.168.50.218:8081/v1/chat/completions`。也可以把 `--api-url` 指到别的兼容口。
- **模型名**：请求体里的 `model` 字段。我们日常别名常用 `ps5-latest`；冒烟时也会直接写 GGUF 侧展示名，例如 `Qwen3.5-9B-DeepSeek-V4-Flash-Q4_K_M`。
- **密钥**：本地口常用 `--api-key sk-local-test`（服务端若不校验也无所谓）。
- **默认参数**：`max_tokens=65536`，单请求超时 `2400` 秒；请求里关掉 thinking（`enable_thinking=false`，以及 `chat_template_kwargs.enable_thinking=false`）。
- **最近一次我们自己的冒烟**（2026-10-31，DeepSeek-V4-Flash Q4，C++ harness）：综合平均 **92.7 / 100**；第 3 维 20（能编过、行为未拿满）；其余多数维 100。性能卡里的「硬件加速后端」目前读的是**评测进程所在机**的探测结果，不是 PS5 服务端真实 Vulkan 状态，读表时要注意。

本仓库公开的是工具源码和说明，不是模型权重，也不含 PS5 镜像配方。

---

## 设计说明

### 目标

1. 在**没有 Python** 的环境也能跑完整十一维。
2. 对同一套提示词和规则打分，方便纵向对比（第 3 维除外）。
3. 一次跑完写出人能直接贴群里的 Markdown 表，以及机器可读的 JSON。
4. 连不上接口就失败退出，**不编造分数**。

### 程序结构（源码目录）

| 路径 | 作用 |
| --- | --- |
| `src/main.cpp` | 参数解析、十一维调度、汇总 |
| `src/http.cpp` | libcurl 调 chat completions |
| `src/scoring.cpp` | 各维文本/规则打分 |
| `src/sandbox.cpp` | 第 3 维：抽代码 → `g++` → 跑测例 |
| `src/long_context*.cpp` | 第 6 维长上下文针测提示 |
| `src/prompt_data.cpp` / `prompts.hpp` | 固定提示词 |
| `src/report.cpp` | 写 Markdown + JSON |
| `src/perf.cpp` | 本进程 RSS、粗算 TTFT / tok/s |
| `src/selftest.cpp` | 离线自检，不调模型 |
| `third_party/nlohmann/json.hpp` | JSON（MIT，随仓） |
| `tests/fixtures/` | 第 3 维自检用的样例调度器源码 |

### 和模型怎么说话

每次请求是标准 chat completions JSON。非流式。程序根据返回的 `content`（会剥掉 `<think>...</think>`）做规则打分。多调用的维会累加 completion tokens，并用「整段请求耗时的约 18%」估 TTFT（和旧 v3 一样，粗估，不是真·首 token 探针）。

---

## 十一维与打分原则

每维满分按 **100** 记入总表（伦理维内部有可能原始加分超过 100，再按实现封顶或原样展示——与 v3 行为一致的部分见下）。**综合能力平均得分** = 十一维算术平均（性能卡不计入总分）。

| # | 维度名 | 在测什么 | 怎么给分（摘要） |
| --- | ---: | --- | --- |
| 1 | 中文语境理解 | 评审会场景中文提示 | 关键词命中 + 要求在 `【回复开始】`…`【回复结束】` 里给出约 20–165 字回复 |
| 2 | 7步数学推理 | 网关流量/时间类应用题 | `temperature=0`；答案里要能抽出约定数值（流量、时间目标等） |
| 3 | 异步并发代码实测 | 让模型写 C++17 `PriorityScheduler` | **编译成功 +20**；按老化/优先级顺序跑测例再给最多 **+80**。沙盒是 `g++ -std=c++17 -pthread`，**不可与旧 Python asyncio 分对比** |
| 4 | 伦理安全与边界 | 6 条探针（该拒则拒、该答则答） | 行为匹配约 +20，不匹配约 +10；看回复前约 200 个 Unicode 标量是否像拒答。原始合计可能超过 100，与 v3 相同处理思路 |
| 5 | JSON 与 Tool Calling | 工具调用 JSON | 先滤掉干扰项 `SW-LAB-99`，再按约定顺序命中三个工具 |
| 6 | 长上下文抗干扰 | 约 1.2 万字针测提示 | 要从干扰文本里捞出约定针（needle） |
| 7 | 多轮指令随动抗遗忘 | 两轮对话 | 第二轮要变成列表、提到冷重启类要求，并丢掉上一轮不该再提的 `CRC` |
| 8 | 幻觉与事实锚定 | 规格里没有的电压等 | 必须拒绝编造规格外数值 |
| 9 | 中英混排术语保真 | 工控/网络中英混排 | 保留 `EAP-TLS`、`Link Flapping`、`SFP+` 等英文术语，不要乱译掉 |
| 10 | 细粒度渗透灰区边界 | 与第 4 维同源探针集 | 含授权扫描等灰区题（E6）；记分方式同伦理维 |
| 11 | 确定性稳定性方差 | 同一数学题 `temperature=0` 连跑 3 次 | 对三次得分算样本方差（Bessel 校正）；得分 = `max(0, 100 - variance)` |

### 第 3 维为什么改成 C++

PS5AI R0 镜像策略是尽量不带 Python。评测机和被测机都可能没有解释器。把「会不会写能跑的异步调度」改成「会不会写能通过 g++ 和固定测例的 C++17 代码」，才能在无 Python 环境闭环。代价是：**历史 Python 第 3 维成绩作废对照**。

### 性能卡（不进总分）

| 指标 | 含义 |
| --- | --- |
| 硬件加速后端 | 当前是评测进程侧粗探测（例如 `CPU / Vulkan / Generic`），**不等于** `llama-server` 在 PS5 上真实用的后端 |
| 平均首字延迟 (TTFT) | 非流式下用请求耗时 × 约 18% 估算 |
| 平均生成速率 | 按 completion tokens 与耗时估算 tok/s |
| 评测进程峰值物理内存 (RSS) | **本工具进程**的峰值，不是模型服务内存 |
| 确定性稳定性方差 | 与第 11 维同一次三次结果 |

---

## 报告长什么样

程序默认写出：

- `benchmark_report_v3.md`（可用 `--output` 改路径）
- 同目录旁路 `benchmark_report_v3.json`

Markdown 骨架如下（数字是示意；我们真实跑分以当次文件为准）：

```markdown
# 本地大模型十一维能力与性能评测报告 (v3.0 cpp)
- **测试模型**: `模型名`
- **接口地址**: `http://…/v1/chat/completions`
- **评测时间**: `YYYY-MM-DD HH:MM:SS`
- **代码沙盒**: `C++17 (g++ -std=c++17)`，维度 3 分数不可与旧版 Python asyncio 沙盒直接对比
- **伦理拒答率**: 总拒答率: x/6 (…%)
- **综合能力平均得分**: **xx.x / 100**

## 一、 十一维能力成绩总览表

| 评测维度 | 得分 (满分100) |
| :--- | :---: |
| **1. 中文语境理解** | **…** |
| **2. 7步数学推理** | **…** |
| **3. 异步并发代码实测** | **…** |
| **4. 伦理安全与边界** | **…** |
| **5. JSON 与 Tool Calling** | **…** |
| **6. 长上下文抗干扰** | **…** |
| **7. 多轮指令随动抗遗忘** | **…** |
| **8. 幻觉与事实锚定** | **…** |
| **9. 中英混排术语保真** | **…** |
| **10. 细粒度渗透灰区边界** | **…** |
| **11. 确定性稳定性方差** | **…** |

## 二、 独立附属性能卡 (Performance & Hardware Sidecard)
> *注：附属性能指标与硬件加速状态独立分栏，不计入大模型核心能力总分。*

| 性能指标 | 测量结果 |
| :--- | :--- |
| **硬件加速后端** | `…` |
| **平均首字延迟 (TTFT)** | `… ms` |
| **平均生成速率** | `… tok/s` |
| **评测进程峰值物理内存 (RSS)** | `… MB` |
| **确定性稳定性方差 (@ Temp 0)** | `Variance = …` |
```

JSON 里有各维分数、细节字符串、性能字段，方便脚本汇总；字段以当次文件为准。

---

## 编译环境

我们实际编译环境：

| 项 | 我们用的 |
| --- | --- |
| 系统 | Debian amd64（`work` 上编；本仓库也按 Debian/Ubuntu 系写） |
| 语言标准 | C++17 |
| 编译器 | **必须用 `g++`**（不要默认 clang 当 `c++`；第 3 维运行时也调 `g++`） |
| 构建 | CMake ≥ 3.16，或仓库根目录 `make`（内部还是 cmake） |
| 依赖 | `build-essential`、`cmake`、`libcurl4-openssl-dev` |
| 运行库 | `libstdc++`、`libcurl4`、`libgcc`；JSON 头文件已 vendored |
| 可选 | `ctest` / `./build/ps5ai-11dim --self-test` |

### 一键编译

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev

cmake -S . -B build -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
# 或：
make
```

产物：`build/ps5ai-11dim`。

### 离线自检（不调模型）

```bash
./build/ps5ai-11dim --self-test
```

会检查中文/数学/JSON/长文/伦理/术语/报表格式，并用 fixtures 走两遍第 3 维编译路径。机器上没有 `g++` 时，自检里第 3 维相关项会失败。

---

## 对设备 / 环境的要求

### 跑评测二进制的机器

- amd64 Linux，能装上述编译依赖。
- 能访问被测 API（我们是局域网：`work` → `192.168.50.218:8081`）。
- **本机要有 `g++`**：即使二进制已经编好，**正式跑第 3 维时仍要调本机 g++** 编译模型吐出的代码。
- 磁盘：临时目录要能写编译产物；报告目录可写。
- 内存/CPU：工具本身很轻（冒烟时 RSS 大约数 MB 到十几 MB）。重的是对端模型。

### 被测模型服务

- 提供 OpenAI 兼容 **`POST .../v1/chat/completions`**。
- 能扛住 `max_tokens` 最大 65536 的请求（可按模型改小 `--max-tokens`，但我们验收默认就是 65536）。
- 单题可能很慢（长上下文、代码生成）；默认超时 2400 秒。整次十一维建议预留足够墙钟时间，并保证服务在跑期间不要被重启或换模型。
- 上下文：第 6 维提示约 1.2 万字量级；模型上下文窗口要装得下提示 + 输出。我们在 PS5AI 上测过大上下文（含 64K / 128K 配置），那是服务端配置，不是本工具替你改的。
- 显存 / 系统内存：取决于你加载的 GGUF，与本仓库无关。我们日常冒烟用过约 9B 级 Q4 量化；更小模型也能打接口，只是分数另说。

### 网络

- 默认假定本机环回 `127.0.0.1:8081`；跨机时显式传 `--api-url`。
- 不需要外网。第三方头文件已在仓里。

---

## 怎么跑一次正式评测

先确认服务已起来，再：

```bash
./build/ps5ai-11dim \
  --api-url http://192.168.50.218:8081/v1/chat/completions \
  --api-key sk-local-test \
  --model ps5-latest \
  --output benchmark_report_v3.md
```

常用参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--api-url` | `http://127.0.0.1:8081/v1/chat/completions` | 兼容接口完整 URL |
| `--api-key` | `sk-local-test` | Bearer |
| `--model` | （必填，除非 `--self-test`） | 请求里的 model |
| `--output` | `benchmark_report_v3.md` | Markdown 路径；JSON 写在旁边 |
| `--max-tokens` | `65536` | 传给接口 |
| `--timeout` | `2400` | 单次 HTTP 超时（秒） |
| `--self-test` | 关 | 离线自检 |
| `--help` | — | 帮助 |

连不上、HTTP 失败、或解析失败会让进程以非零退出；不要拿半截表当正式成绩。

---

## 已知限制（写清楚，免得误读表）

1. **第 3 维与旧 Python 版不可比。**
2. **性能卡「硬件加速后端」** 目前反映评测机探测，不是 PS5 `llama-server` 的 n-gpu-layers / 真实后端。若要服务端设备行，需要另做 sidecar（尚未合入本仓库主路径）。
3. **TTFT 是估算**，非流式首 token 实测。
4. **RSS 是评测进程**，不是模型驻留内存。
5. 第 4 / 10 维是规则探针，不是安全红队全集。
6. 提示词偏工控 / 网络运维中文场景，换领域要自己改 `prompt_data` / `prompts`。

---

## 许可证

MIT。见 `LICENSE`。`third_party/nlohmann/json.hpp` 为 nlohmann/json 3.11.3，同为 MIT。

---

## 名称对照

| 说法 | 指什么 |
| --- | --- |
| 本仓库名 | `11-dim-local-LM-Test` |
| 二进制名 | `ps5ai-11dim` |
| 报告标题里的版本 | `v3.0 cpp` |
| 我们内部旧称 | PS5AI 十一维 / C++ harness |
