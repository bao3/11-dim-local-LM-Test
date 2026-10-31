# 跨发行版自包含评测包设计

日期：2026-10-31  
仓库：`bao3/11-dim-local-LM-Test`  
状态：已口头确认 A 方案；本文供实现前最终审阅

## 背景与目标

当前文档与构建默认 Debian（`apt` / `build-essential` / `/usr/bin/g++`）。实际评测机可能是 Ubuntu、Fedora、CentOS、Arch、Alpine 等。目标：

1. **从源码编译**：各发行版有明确依赖与包名。
2. **Release 二进制**：尽量一个包在各发行版直接跑。
3. **第 3 维**：不依赖本机是否已装编译器；Release 附带便携 `g++` 工具链，解压即用、不改系统。
4. **发版**：用 GitHub Actions 打下一版；**仅当 GitHub 上出现新的 commit 时才触发重新编译**（不为同一 commit 重复空跑）。

非目标：二进制内探测发行版并 `sudo apt/dnf/...` 装包；为每个发行版维护多套动态链接产物。

## 方案总览（已选）

自包含 Release 归档 + 编译器探测链 + 多发行版 README 装包表。日常开发仍可用系统动态 `libcurl` + 系统编译器。

## 构建与依赖

### 开发构建（本机）

- 保持现有 CMake/`g++` 路径；`find_package(CURL)` 动态链接即可。
- README 增加装包对照（示意，实现时按实测校对）：

| 发行版 | 编译器 / 构建 | libcurl 开发包 |
|--------|---------------|----------------|
| Debian / Ubuntu | `build-essential` `cmake` | `libcurl4-openssl-dev` |
| Fedora | `gcc-c++` `cmake` `make` | `libcurl-devel` |
| CentOS / RHEL | `gcc-c++` `cmake` `make` | `libcurl-devel` |
| Arch | `base-devel` `cmake` | `curl` |
| Alpine | `g++` `cmake` `make` | `curl-dev` |

### Release 构建（CI）

- 产物尽量**自包含**：优先静态链接 `libcurl`、OpenSSL（或等价）、`libstdc++`/`libgcc`；若无法完全静态，则仅依赖足够老的 glibc，并在文档标明最低要求。
- **不做**运行时包管理器安装。
- HTTP 层短期可继续 libcurl（静态）；若静态 curl 过重，可另开后续任务改为内嵌 HTTP(S) 客户端，不阻塞本设计主路径。

## 第 3 维编译器探测

解析顺序（找到第一个可执行即停）：

1. 环境变量 `PS5AI_CXX`；若未设则兼容旧名 `PS5AI_GXX`
2. 包内便携工具链：相对二进制位置的 `../toolchain/bin/g++`（归档布局见下），或 `PS5AI_TOOLCHAIN` 指向的根目录下 `bin/g++`
3. `/usr/bin/g++` → `PATH` 中的 `g++` → `clang++` → `c++`

行为：

- 编译参数保持 `-std=c++17 -pthread`（及现有沙盒限制）。
- 报告 Markdown / JSON 记录实际编译器路径与 `--version` 首行；JSON 增加字段如 `dim3_compiler`、`dim3_compiler_version`。
- 全部找不到：details 标 `[ENV_ERROR]`，进程退出码 `3`（与现有语义一致）。
- **便携链优先于系统编译器**，减少跨机分数漂移；用户仍可用 `PS5AI_CXX` 强制指定。

便携工具链来源（实现选定其一并写进 CI 注释）：

- 官方可再分发的 linux-x86_64 工具链（例如 xpack / 官方 GCC 发行包 / 经许可的 crosstool 产物），版本钉死，校验 sha256。
- 仅含运行第 3 维所需：`g++`、cc1plus、汇编链接器、C++17 头与 libstdc++；不追求完整 IDE 环境。

## Release 资产与布局

下一版本号：**1.1**（tag `v1.1`）。**不覆盖**已发布的 `v1.0`。

归档示例名：`ps5ai-11dim-linux-amd64.tar.gz`，解压后：

```text
ps5ai-11dim-linux-amd64/
  ps5ai-11dim          # 评测主程序
  toolchain/           # 便携 g++ 树（bin/lib/include…）
  SHA256SUMS
  README-run.txt       # 解压即跑的最短说明
```

用法：解压后 `./ps5ai-11dim ...`；无需系统 `g++`。仍可用 `PS5AI_CXX` 覆盖。

另附 `.sha256` 或归档内 `SHA256SUMS`。可选保留「仅二进制、无工具链」资产，默认以**含工具链的归档**为推荐下载项。

## GitHub Actions

工作流（例如 `.github/workflows/release.yml`）：

**触发（必须满足「有新 commit」）：**

- `push` 到 tag `v*`（tag 指向的 commit 为新发布点）；和/或
- `push` 到 `main` 且 **paths** 命中会改变产物的路径（如 `src/**`、`CMakeLists.txt`、`toolchain/**`、工作流与打包脚本），用于自动打预览包或推进版本——**实现时二选一写死**：本设计默认 **仅 tag `v*` 发布正式 Release**；`main` 上同路径变更可只跑 `build + self-test` 不做 Release，避免无 tag 也发版。
- **禁止**：`schedule` 定时重编；对同一 commit 重复 `workflow_dispatch` 覆盖正式资产（若保留手工触发，须要求输入新 tag 或拒绝在无新 commit 时上传 Release）。
- **禁止**：仅改 README 文档且 paths-ignore 已排除时触发正式重编（文档-only push 不编译）。

**任务概要：**

1. 检出仓库（该 commit）。
2. 下载并校验钉死版本的便携工具链，放到打包目录 `toolchain/`。
3. 配置静态/自包含 CMake 构建，编译 `ps5ai-11dim`。
4. 跑 `./ps5ai-11dim --self-test`（CI 镜像内可用系统或刚下的工具链）。
5. 组装 tar.gz + SHA256，创建 GitHub Release（tag 名，如 `v1.1`），上传资产。

版本 bump：由打 tag 的人决定；Actions 不在无新 commit 时自动改版本号或重传 `v1.0`。

## 文档与兼容说明

- README：推荐 Release 自包含包；保留源码安装表；说明第 3 维编译器探测顺序与 `PS5AI_CXX`。
- 报告文案：由「仅 g++」改为「C++17 编译器（便携 g++ 或本机）」，并打印实际路径。
- 分数：同版本便携链下跨发行版可比；若用户强制本机 `clang++`，接受轻微差异（已确认）。

## 错误处理

| 情况 | 行为 |
|------|------|
| 无编译器 | `[ENV_ERROR]`，退出 3 |
| 工具链损坏/缺文件 | 同左，details 写明路径 |
| 静态二进制在极老 glibc 上失败 | README 写明最低 glibc；用户改从源码编 |
| Alpine musl 与「仅静态 glibc」冲突 | 优先真正静态（无动态 loader 依赖）或文档标明 Alpine 需源码构建；实现阶段在 CI 用 `ldd`/`file` 验收 |

## 测试计划

- CI：`--self-test` 必须 PASS（含第 3 维两次编译路径，使用打包工具链）。
- 手工抽查：至少在 Debian 与非 Debian（如 Fedora 或 Arch 容器）解压归档，无系统 `g++` 时第 3 维自检或 dry 编译成功。
- 确认改文档-only 的 commit **不**触发正式 Release 重编。
- 确认 `v1.1` 发布后 `v1.0` 资产仍在。

## 范围外（后续可做）

- 内嵌 HTTP 客户端去掉 libcurl。
- 多架构（arm64）第二套归档。
- 把便携工具链做成可选二次下载以缩小默认包。
