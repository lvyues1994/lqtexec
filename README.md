# lqtexec

用 C++17 的 [lexec](https://github.com/lvyues1994/lexec) 封装 Qt，让 Qt 程序里的异步来源参与 sender/receiver 组合。支持 Qt 5.15 与 Qt 6。

设计、不变量与实施计划见 [docs/design.md](docs/design.md)。

## 构建

需要 CMake 3.25+、Ninja、GCC 10+ 或 Clang 12+，以及 Qt 5.15 或 Qt 6 的 Qt::Core。lexec 的来源按顺序取：父项目已有的 `lexec::lexec` 目标、环境变量或缓存变量 `LQTEXEC_LEXEC_SOURCE_DIR` 指向的本地源码，否则用 FetchContent 拉取固定提交 `e032d551d53d87209b83d07fd172890461db643b`。

```sh
export LQTEXEC_LEXEC_SOURCE_DIR=/home/lvyues/code/exec/lexec
export CMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64   # Qt 6 不在系统路径时
cmake --workflow --preset gcc-debug
```

可用的 preset 为 `{gcc,clang}-{debug,asan,tsan}`、`gcc-release`、`gcc-noexcept`（Qt 6），以及 `gcc-debug-qt5`、`gcc-noexcept-qt5`、`clang-asan-qt5`（Qt 5）。`LQTEXEC_QT_MAJOR_VERSION` 留空时优先 Qt 6。作为子项目使用时链接 `lqtexec::lqtexec`，测试默认关闭。
