这个目录在 git 里是空的。便携 `g++` 用 `scripts/fetch-toolchain.sh` 下载到你指定的目录（打包脚本再把它拷进 Release），不要把整棵工具链提交进来。

当前钉的是 xPack GCC 14.3.0-1 linux-x64。sha256 取自上游发布页的 `.sha` 附件，写在 `scripts/fetch-toolchain.sh` 里。计划草稿里的 `v14.2.0-3` 地址已经 404，所以没有用那一版。
