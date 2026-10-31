ps5ai-11dim linux-amd64 自包含包

在这个目录里直接跑：

  ./ps5ai-11dim --self-test
  ./ps5ai-11dim --model <名字> --api-url <http://主机:端口/v1/chat/completions>

第 3 维用 -std=c++17 -pthread。编译器先看环境变量 PS5AI_CXX，没有再看 PS5AI_GXX，再看 PS5AI_TOOLCHAIN/bin/g++，然后用本目录 toolchain/bin/g++，最后才找系统的 g++、clang++、c++。不想用包里的 g++，就把 PS5AI_CXX 指到你自己的编译器。

程序不会自己去跑 apt、dnf、pacman、apk。

这个二进制在 GitHub 的 ubuntu-latest 上编译，还动态链接 glibc 和 libcurl（libstdc++ / libgcc 在 Release 里尽量静态链上）。glibc 太老、或者 Alpine 这种 musl，加载失败就改从源码编。用 ldd ./ps5ai-11dim 可以看还依赖哪些库。

工具链是 xPack GCC 14.3.0-1 linux-x64，许可证跟 GCC 走，不是本仓库的 MIT。下载地址和 sha256 写在仓库 scripts/fetch-toolchain.sh 里。

同目录的 SHA256SUMS 是包内文件的校验。发布页上另有一份 SHA256SUMS，那一份是对 tar.gz 本身的。
