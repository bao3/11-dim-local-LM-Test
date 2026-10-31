# Cross-distro portable toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a self-contained `v1.1` Release (binary + portable `g++` toolchain) that runs dimension 3 across Debian/Ubuntu/Fedora/CentOS/Arch/Alpine-class hosts without system package installs, rebuilt only when GitHub sees a new commit (tag `v*`).

**Architecture:** Extend `gxx_binary()` into a documented probe chain (env → bundled toolchain beside the executable → system compilers). GitHub Actions on tag `v*` builds a preferably static/self-contained `ps5ai-11dim`, downloads a pinned portable GCC tree into `toolchain/`, packs `ps5ai-11dim-linux-amd64.tar.gz`, and uploads a GitHub Release. `main` pushes that only touch docs do not release; code pushes run build+self-test without publishing.

**Tech Stack:** C++17, CMake, libcurl, GitHub Actions, pinned xpack (or equivalent) linux-x64 GCC tarball, `tar`/`sha256sum`.

## Global Constraints

- Do not overwrite Release `v1.0` assets; next release is **`v1.1`**.
- Do not run `apt`/`dnf`/`pacman`/`apk` from the harness binary.
- Dim3 flags stay `-std=c++17 -pthread` (plus existing sandbox limits).
- Compiler probe order: `PS5AI_CXX` → `PS5AI_GXX` → `$BUNDLE/toolchain/bin/g++` / `$PS5AI_TOOLCHAIN/bin/g++` → `/usr/bin/g++` → `g++` → `clang++` → `c++`.
- Actions: formal Release only on **new commit** via tag `v*`; no schedule; docs-only paths must not rebuild Release.
- Programmer does not assemble/dd PS5 images.
- User-facing README stays plain Chinese (humanizer tone); commands in code spans.

## File map

| Path | Role |
|------|------|
| `src/sandbox.cpp` / `sandbox.hpp` | Resolve C++ compiler; expose last-resolved path/version helpers |
| `src/paths.hpp` + `src/paths.cpp` (new) | Resolve executable dir via `/proc/self/exe` |
| `src/report.hpp` / `report.cpp` | Emit `dim3_compiler` / `dim3_compiler_version` |
| `src/main.cpp` | Fill report fields; print compiler line at start |
| `src/selftest.cpp` | Probe-order + report JSON field checks |
| `CMakeLists.txt` | Optional static-libstdc++ / static curl flags for Release CI |
| `scripts/fetch-toolchain.sh` | Download+verify pinned portable GCC |
| `scripts/pack-release.sh` | Assemble tarball + SHA256SUMS + README-run.txt |
| `.github/workflows/ci.yml` | `push`/`pull_request` on code paths → build + `--self-test` |
| `.github/workflows/release.yml` | `push` tags `v*` → fetch toolchain, pack, `gh release create` |
| `README.md` | Multi-distro deps + Release bundle instructions |
| `toolchain/.gitkeep` + `toolchain/README.md` | Local placeholder; real tree is CI/download only (not committed) |

---

### Task 1: Executable-dir helper + compiler probe

**Files:**
- Create: `src/paths.hpp`, `src/paths.cpp`
- Modify: `src/sandbox.hpp`, `src/sandbox.cpp` (`gxx_binary` → `resolve_cxx_compiler`)
- Modify: `CMakeLists.txt` (add `src/paths.cpp`)
- Test: via `src/selftest.cpp` in Task 2

**Interfaces:**
- Produces: `std::string executable_dir();` — directory containing the running binary (Linux: `readlink("/proc/self/exe")` then dirname). Empty string if unavailable.
- Produces: `std::string resolve_cxx_compiler();` — probe order in Global Constraints; returns path string even if not yet proven executable (caller still runs it); prefers first path with `access(X_OK)==0`, else last fallback name `"c++"`.
- Produces: `std::string last_resolved_cxx();` and `std::string cxx_version_line(const std::string& cxx);` — version via `run_process({cxx,"--version"})` first stdout line, truncated to 200 chars; empty on failure.
- Consumes: existing `run_process` in `sandbox.cpp` (keep internal or declare in hpp if needed for version only inside sandbox.cpp).

- [ ] **Step 1: Add `paths` helpers**

```cpp
// src/paths.hpp
#pragma once
#include <string>
std::string executable_dir();
```

```cpp
// src/paths.cpp
#include "paths.hpp"
#include <limits.h>
#include <unistd.h>
#include <string>

std::string executable_dir() {
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    std::string path(buf);
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}
```

- [ ] **Step 2: Replace `gxx_binary` with probe**

In `sandbox.cpp`, replace `gxx_binary()` body with:

```cpp
#include "paths.hpp"

static std::string g_last_cxx;

static bool is_executable(const std::string& p) {
    return !p.empty() && access(p.c_str(), X_OK) == 0;
}

std::string resolve_cxx_compiler() {
    if (const char* env = std::getenv("PS5AI_CXX")) {
        if (env[0] != '\0' && is_executable(env)) {
            return g_last_cxx = env;
        }
    }
    if (const char* env = std::getenv("PS5AI_GXX")) {
        if (env[0] != '\0' && is_executable(env)) {
            return g_last_cxx = env;
        }
    }
    if (const char* root = std::getenv("PS5AI_TOOLCHAIN")) {
        if (root[0] != '\0') {
            std::string cand = std::string(root) + "/bin/g++";
            if (is_executable(cand)) {
                return g_last_cxx = cand;
            }
        }
    }
    const std::string exe_dir = executable_dir();
    if (!exe_dir.empty()) {
        std::string cand = exe_dir + "/toolchain/bin/g++";
        if (is_executable(cand)) {
            return g_last_cxx = cand;
        }
        // tarball layout: .../ps5ai-11dim-linux-amd64/ps5ai-11dim + .../toolchain/
        cand = exe_dir + "/../toolchain/bin/g++";
        if (is_executable(cand)) {
            return g_last_cxx = cand;
        }
    }
    const char* candidates[] = {"/usr/bin/g++", "g++", "clang++", "c++"};
    for (const char* c : candidates) {
        if (c[0] == '/') {
            if (is_executable(c)) {
                return g_last_cxx = c;
            }
        } else {
            // PATH lookup: try access after which-like — use run_process({"sh","-c","command -v ..."})
            // Minimal: if access on bare name fails, still accept name for execvp PATH search when
            // command -v succeeds. Implement command -v check:
            ProcResult which = run_process({"/bin/sh", "-c", std::string("command -v ") + c},
                                           /*short timeout opts*/);
            // Prefer a small helper that only checks exit 0; if awkward, try exec of `c` with --version later.
            // Practical approach for this codebase: check common prefixes then return first name
            // and let run_process fail into ENV_ERROR.
        }
    }
    // Concrete implementation to use (clearer than the comment above):
    if (is_executable("/usr/bin/g++")) return g_last_cxx = "/usr/bin/g++";
    if (is_executable("/usr/bin/clang++")) return g_last_cxx = "/usr/bin/clang++";
    if (is_executable("/usr/bin/c++")) return g_last_cxx = "/usr/bin/c++";
    // PATH fallbacks (execvp resolves):
    for (const char* c : {"g++", "clang++", "c++"}) {
        ProcResult ver = run_process({c, "--version"}, /*opts: timeout 5s, no rlimit needed*/);
        if (ver.exit_code == 0 || ver.spawned) {
            // If process started, treat as found even if version failed oddly.
            // Prefer: spawn success and exit 0.
            if (ver.exit_code == 0) {
                return g_last_cxx = c;
            }
        }
    }
    return g_last_cxx = "c++";
}

std::string last_resolved_cxx() { return g_last_cxx.empty() ? resolve_cxx_compiler() : g_last_cxx; }

std::string cxx_version_line(const std::string& cxx) {
    ProcResult ver = run_process({cxx, "--version"}, /*5s*/);
    if (ver.exit_code != 0) return {};
    auto pos = ver.stdout_text.find('\n');
    std::string line = pos == std::string::npos ? ver.stdout_text : ver.stdout_text.substr(0, pos);
    if (line.size() > 200) line.resize(200);
    return line;
}
```

Wire all former `gxx_binary()` call sites to `resolve_cxx_compiler()`. Update ENV_ERROR strings from 「未找到 g++」 to 「未找到 C++ 编译器（…）」.

Declare in `sandbox.hpp`:

```cpp
std::string resolve_cxx_compiler();
std::string last_resolved_cxx();
std::string cxx_version_line(const std::string& cxx);
```

- [ ] **Step 3: CMake add `paths.cpp`**

```cmake
add_executable(ps5ai-11dim
    ...
    src/paths.cpp
    src/sandbox.cpp
    ...
)
```

- [ ] **Step 4: Build and smoke**

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++ && cmake --build build -j"$(nproc)"
./build/ps5ai-11dim --help | head
```

Expected: builds; help still prints.

- [ ] **Step 5: Commit**

```bash
git add src/paths.hpp src/paths.cpp src/sandbox.hpp src/sandbox.cpp CMakeLists.txt
git commit -m "feat: resolve dim3 compiler via env, bundle, then system"
```

---

### Task 2: Report fields + self-test for probe

**Files:**
- Modify: `src/report.hpp`, `src/report.cpp`, `src/main.cpp`, `src/selftest.cpp`

**Interfaces:**
- Consumes: `resolve_cxx_compiler`, `cxx_version_line`, `last_resolved_cxx`
- Produces: `ReportInput::dim3_compiler`, `ReportInput::dim3_compiler_version` strings in Markdown + JSON

- [ ] **Step 1: Extend ReportInput**

```cpp
std::string dim3_compiler;
std::string dim3_compiler_version;
```

In `build_report`, Markdown line:

```text
- **维度 3 编译器**: `<path>` (`<version>`)
```

JSON:

```cpp
root["dim3_compiler"] = in.dim3_compiler;
root["dim3_compiler_version"] = in.dim3_compiler_version;
root["dim3_note"] = "Dimension 3 compiles C++17 (-std=c++17 -pthread) with the resolved compiler (bundled g++ preferred). Not comparable to the old Python asyncio sandbox.";
```

- [ ] **Step 2: main.cpp fill fields**

Before dim loop (or when building ReportInput):

```cpp
const std::string cxx = resolve_cxx_compiler();
report_in.dim3_compiler = cxx;
report_in.dim3_compiler_version = cxx_version_line(cxx);
std::cout << "维度 3 编译器: " << cxx;
if (!report_in.dim3_compiler_version.empty()) {
    std::cout << " (" << report_in.dim3_compiler_version << ")";
}
std::cout << "\n";
```

Remove/replace the hard-coded 「维度 3 沙盒: g++ …」 line accordingly.

- [ ] **Step 3: Self-test — JSON keys + env override**

In `selftest.cpp`, after building a tiny report or parsing existing fixture path:

```cpp
{
    ReportInput in;
    in.model = "t";
    in.api_url = "http://127.0.0.1/v1/chat/completions";
    in.timestamp = "t";
    in.dim3_compiler = "/tmp/fake-g++";
    in.dim3_compiler_version = "g++ (test) 1.0";
    // minimal scores so build_report works — copy pattern from existing selftest report check
    Report r = build_report(in);
    auto parsed = nlohmann::json::parse(r.json);
    CHECK(parsed["dim3_compiler"] == "/tmp/fake-g++");
    CHECK(parsed["dim3_compiler_version"] == "g++ (test) 1.0");
}
{
    // Probe: PS5AI_CXX wins when executable
    // Create a small shell wrapper as fake compiler that accepts --version and -std=c++17 no-op? 
    // Too heavy for full compile. Instead only assert resolve_cxx_compiler() returns getenv when
    // pointing at /usr/bin/g++ (skip if missing).
    if (access("/usr/bin/g++", X_OK) == 0) {
        setenv("PS5AI_CXX", "/usr/bin/g++", 1);
        CHECK(resolve_cxx_compiler() == "/usr/bin/g++");
        unsetenv("PS5AI_CXX");
    }
}
```

Keep existing dim3 fixture compile tests unchanged (still need a real compiler on the machine).

- [ ] **Step 4: Run self-test**

```bash
cmake --build build -j"$(nproc)" && ./build/ps5ai-11dim --self-test
```

Expected: `PASS` / exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/report.hpp src/report.cpp src/main.cpp src/selftest.cpp
git commit -m "feat: record dim3 compiler path and version in reports"
```

---

### Task 3: Fetch + pack scripts

**Files:**
- Create: `scripts/fetch-toolchain.sh`, `scripts/pack-release.sh`, `toolchain/README.md`
- Create: `packaging/README-run.txt` (template copied into tarball)

**Interfaces:**
- Produces: `scripts/fetch-toolchain.sh <dest_dir>` downloads pinned GCC, verifies sha256, extracts so `<dest_dir>/bin/g++` exists.
- Produces: `scripts/pack-release.sh <build_dir> <toolchain_dir> <out_dir>` writes `ps5ai-11dim-linux-amd64.tar.gz` + `SHA256SUMS`.

Pin (verify at implement time; replace URL/sha if upstream moved):

```bash
# Example pin — implementer MUST re-check release assets and sha256 before merging:
TOOLCHAIN_URL="https://github.com/xpack-dev-tools/gcc-xpack/releases/download/v14.2.0-3/xpack-gcc-14.2.0-3-linux-x64.tar.gz"
TOOLCHAIN_SHA256="<fill from upstream release checksums>"
```

If xpack layout nests `xpack-gcc-.../bin/g++`, `fetch-toolchain.sh` must flatten or symlink so `dest/bin/g++` works.

`pack-release.sh` layout:

```text
$OUT/ps5ai-11dim-linux-amd64/
  ps5ai-11dim          # stripped
  toolchain/           # contents of fetch dest
  SHA256SUMS
  README-run.txt
```

Relative probe `exe_dir/toolchain/bin/g++` matches this layout (binary and `toolchain/` siblings).

- [ ] **Step 1: Write fetch script** with `set -euo pipefail`, curl -L, sha256 check, extract.

- [ ] **Step 2: Write pack script** copying binary + toolchain + README-run.txt; `strip` binary; `tar czf`; write SHA256SUMS.

- [ ] **Step 3: Local dry-run** (if network allows):

```bash
bash scripts/fetch-toolchain.sh /tmp/ps5ai-tc
test -x /tmp/ps5ai-tc/bin/g++
cmake -S . -B build-rel -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
bash scripts/pack-release.sh build-rel /tmp/ps5ai-tc /tmp/ps5ai-out
tar tzf /tmp/ps5ai-out/ps5ai-11dim-linux-amd64.tar.gz | head
```

Expected: archive lists `ps5ai-11dim-linux-amd64/ps5ai-11dim` and `.../toolchain/bin/g++`.

- [ ] **Step 4: Commit**

```bash
git add scripts/fetch-toolchain.sh scripts/pack-release.sh packaging/README-run.txt toolchain/README.md
git commit -m "chore: add toolchain fetch and release pack scripts"
```

---

### Task 4: CMake Release hardening (best-effort static)

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add option**

```cmake
option(PS5AI_STATIC_CXX "Link libstdc++/libgcc statically" OFF)
option(PS5AI_PREFER_STATIC_CURL "Prefer static libcurl if available" OFF)

if(PS5AI_STATIC_CXX)
  target_link_options(ps5ai-11dim PRIVATE -static-libstdc++ -static-libgcc)
endif()
```

Document in README that CI sets `-DPS5AI_STATIC_CXX=ON`. Full musl static is best-effort; if static curl is painful, ship dynamic curl but static libstdc++ first and note glibc floor in README-run.txt.

- [ ] **Step 2: Build with flag and `ldd`**

```bash
cmake -S . -B build-static -DCMAKE_CXX_COMPILER=g++ -DPS5AI_STATIC_CXX=ON
cmake --build build-static -j"$(nproc)"
ldd build-static/ps5ai-11dim || true
./build-static/ps5ai-11dim --self-test
```

Expected: self-test PASS; `libstdc++` not in `ldd` when static cxx works.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: optional static libstdc++ for portable Release"
```

---

### Task 5: GitHub Actions CI + Release

**Files:**
- Create: `.github/workflows/ci.yml`, `.github/workflows/release.yml`

**Interfaces:**
- `ci.yml`: on `push`/`pull_request` to `main`, paths: `src/**`, `CMakeLists.txt`, `tests/**`, `scripts/**`, `.github/workflows/**` — install deps, build, `--self-test`. **paths-ignore** alone is insufficient; use positive `paths` so README-only commits skip.
- `release.yml`: on `push` tags matching `v*` only (implies new commit pointed by tag). Jobs: checkout that commit → fetch toolchain → cmake Release + `PS5AI_STATIC_CXX=ON` → self-test with `PS5AI_TOOLCHAIN` pointing at fetched tree → pack → `gh release create "$TAG" ... --title ... --notes ...` upload tarball + SHA256SUMS. No `schedule`. `workflow_dispatch` optional but must refuse to upload if tag already exists / or require typing tag name that points at HEAD with a new commit — **simplest: no workflow_dispatch**.

- [ ] **Step 1: Write `ci.yml`**

```yaml
name: ci
on:
  push:
    branches: [main]
    paths:
      - 'src/**'
      - 'tests/**'
      - 'CMakeLists.txt'
      - 'Makefile'
      - 'scripts/**'
      - '.github/workflows/**'
  pull_request:
    branches: [main]
    paths:
      - 'src/**'
      - 'tests/**'
      - 'CMakeLists.txt'
      - 'Makefile'
      - 'scripts/**'
      - '.github/workflows/**'
jobs:
  selftest:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y build-essential cmake libcurl4-openssl-dev
      - run: cmake -S . -B build -DCMAKE_CXX_COMPILER=g++ && cmake --build build -j$(nproc)
      - run: ./build/ps5ai-11dim --self-test
```

- [ ] **Step 2: Write `release.yml`**

```yaml
name: release
on:
  push:
    tags: ['v*']
jobs:
  release:
    runs-on: ubuntu-latest
    permissions:
      contents: write
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y build-essential cmake libcurl4-openssl-dev
      - run: bash scripts/fetch-toolchain.sh "$HOME/ps5ai-toolchain"
      - run: |
          cmake -S . -B build -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release -DPS5AI_STATIC_CXX=ON
          cmake --build build -j$(nproc)
      - run: PS5AI_TOOLCHAIN="$HOME/ps5ai-toolchain" ./build/ps5ai-11dim --self-test
      - run: bash scripts/pack-release.sh build "$HOME/ps5ai-toolchain" dist
      - name: Publish GitHub Release
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          TAG="${GITHUB_REF_NAME}"
          gh release create "$TAG" dist/*.tar.gz dist/SHA256SUMS \
            --title "$TAG" \
            --notes "Self-contained linux-amd64 bundle with portable g++ toolchain for dimension 3. Rebuilds only when this tag points at a new commit."
```

- [ ] **Step 3: Commit workflows**

```bash
git add .github/workflows/ci.yml .github/workflows/release.yml
git commit -m "ci: self-test on code pushes; Release bundle on v* tags"
```

---

### Task 6: README multi-distro + runbook

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace Debian-only install block** with table from the design spec (apt/dnf/yum/pacman/apk).
- [ ] **Step 2: Add「推荐：Release 自包含包」** — download `ps5ai-11dim-linux-amd64.tar.gz`, extract, run `./ps5ai-11dim`; note bundled toolchain; `PS5AI_CXX` / `PS5AI_TOOLCHAIN` overrides.
- [ ] **Step 3: Note Actions** — tags `v*` publish; docs-only commits do not rebuild.
- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: multi-distro deps and portable Release instructions"
```

---

### Task 7: Tag `v1.1` and verify Actions

**Files:** none (git tag + gh)

- [ ] **Step 1: Ensure all Tasks 1–6 are on `origin/main`**
- [ ] **Step 2: Tag annotated release on that commit**

```bash
git tag -a v1.1 -m "v1.1 self-contained bundle + portable g++"
git push origin v1.1
```

- [ ] **Step 3: Watch workflow**

```bash
gh run list --workflow=release.yml -L 5
gh run watch
gh release view v1.1
```

Expected: green run; assets include `ps5ai-11dim-linux-amd64.tar.gz` and checksums; `v1.0` still present.

- [ ] **Step 4: Optional smoke** — download asset, extract on a machine/container without trusting system g++ first (`PATH=/usr/bin` with g++ hidden or `PS5AI_TOOLCHAIN` only), run `--self-test`.

---

## Spec coverage checklist

| Spec item | Task |
|-----------|------|
| Multi-distro source deps in README | 6 |
| Self-contained Release binary | 4, 5, 7 |
| Bundled portable g++ | 3, 5 |
| Probe order + ENV_ERROR | 1, 2 |
| Report compiler fields | 2 |
| Actions only on new commit / tag `v*` | 5, 7 |
| No package-manager from binary | 1 (no code path) |
| Do not clobber v1.0 | 7 (`v1.1`) |

## Placeholder / pin note

`TOOLCHAIN_URL` / `TOOLCHAIN_SHA256` in Task 3 must be filled from the live upstream checksum page at implementation time — do not invent a sha.
