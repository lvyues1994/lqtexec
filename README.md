# lqtexec

用 C++17 的 [lexec](https://github.com/lvyues1994/lexec) 封装 Qt，让 Qt 的事件循环线程、定时器、信号和对象生命周期参与 sender/receiver 组合。支持 Qt 5.15 与 Qt 6。

设计、不变量与实施计划见 [docs/design.md](docs/design.md)。

## 构建

需要 CMake 3.25+、Ninja、GCC 10+ 或 Clang 12+，以及 Qt 5.15 或 Qt 6 的 Qt::Core。lexec 的来源按顺序取：父项目已有的 `lexec::lexec` 目标、环境变量或缓存变量 `LQTEXEC_LEXEC_SOURCE_DIR` 指向的本地源码，否则用 FetchContent 拉取固定提交 `e032d551d53d87209b83d07fd172890461db643b`。

```sh
export LQTEXEC_LEXEC_SOURCE_DIR=/home/lvyues/code/exec/lexec
export CMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64   # Qt 6 不在系统路径时
cmake --workflow --preset gcc-debug
```

可用的 preset 为 `{gcc,clang}-{debug,asan,tsan}`、`gcc-release`、`gcc-noexcept`（Qt 6），以及 `gcc-debug-qt5`、`gcc-noexcept-qt5`、`clang-asan-qt5`（Qt 5）。`LQTEXEC_QT_MAJOR_VERSION` 留空时优先 Qt 6。作为子项目使用时链接 `lqtexec::lqtexec`，测试和示例默认关闭。

## 使用

```cpp
#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>
#include <lqtexec/ExecWithScope.h>
#include <lqtexec/ObjectScope.h>
#include <lqtexec/WaitSignal.h>

int main(int argc, char **argv) {
    auto app = QCoreApplication{argc, argv};
    auto gui = lqtexec::EventLoopContext{};                    // 主线程的事件循环
    auto worker = lqtexec::EventLoopThread{QStringLiteral("worker")};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};

    auto timer = QTimer{};
    timer.start(100);
    lexec::spawn(lqtexec::wait_signal(&timer, &QTimer::timeout)   // 等一次 timeout
                     | lexec::continues_on(worker.get_scheduler()) // 到工作线程上计算
                     | lexec::then([]() noexcept { return 42; })
                     | lexec::continues_on(gui.get_scheduler())    // 回到主线程
                     | lexec::then([&](int) noexcept { stop.request_stop(); })
                     | lexec::upon_error([](auto const &) noexcept {}),
                 scope.get_token());

    return lqtexec::exec_with_scope(app, scope, stop);          // 停止 → 排空 → 退出
}
```

完整示例见 [examples/console_demo.cpp](examples/console_demo.cpp)，其中用 `ObjectScope` 把工作绑定到一个 QObject 的生命周期。

## API

| 入口 | 行为 |
| --- | --- |
| `EventLoopContext{}` / `EventLoopContext{thread}` | 把当前线程或给定 `QThread` 的事件循环作为执行资源 |
| `EventLoopThread{name}` | 自带事件循环的线程，析构时退出并等待线程 |
| `lexec::schedule(sch)` | 在该事件循环上异步完成；完成为 value 或 stopped |
| `schedule_after(sch, duration)` / `schedule_at(sch, time_point)` | `steady_clock` 定时器；停止请求立即以 stopped 完成 |
| `wait_signal(obj, &T::signal)` | 下一次发射的参数（按声明类型，引用转发）；对象为空、被销毁或停止时为 stopped |
| `SignalChannel{obj, &T::signal, options}` / `channel.next()` | 只连接一次、缓冲多次发射；`next()` 交付最早的一次（衰变复制）；可设容量与溢出策略 |
| `ThreadPoolScheduler{pool, priority}` | 在 `QThreadPool` 上执行，与 QtConcurrent 共用线程池；停止请求取回排队中的任务 |
| `ObjectScope{owner, sch, parent.get_token()}` | 绑定对象生命周期的 scope；析构不阻塞；父 scope 停止时一并收束 |
| `scope.scheduler()` / `then_on(scope, fn)` | 回到对象线程，对象已析构则以 stopped 跳过 |
| `exec_with_scope(app, scope, stop)` | 运行事件循环，停止后排空 scope 再返回退出码 |

使用要点：

- `spawn` 只接受错误已处理的工作；开启异常时 `wait_signal` 与跨线程 hop 都可能报告 `exception_ptr`，用 `upon_error` 处理。
- `wait_signal` 是边沿触发的：start 之前的发射会丢失。多次发射的信号（进度、`readyRead`）用 `SignalChannel` 配合 `lexec::repeat_until`。
- 不要在某个上下文的线程上对需要该上下文的 sender 调用阻塞的 `lexec::sync_wait`。
- 碰 QObject 成员的函数应紧跟在 hop 到 `ObjectScope::scheduler()` 之后（或用 `then_on`）。

## 测试与已知限制

测试覆盖调度顺序、嵌套事件循环、定时器、跨线程的恰好一次完成、对象生命周期与收束。本地在 Qt 6.8.3 与 Qt 5.15.13 上以 GCC 13 / Clang 18 的 debug、ASan+UBSan、无异常构建通过。

TSan 下测试全部通过，但会报告来自 Qt 与 glib 内部同步的数据竞争（投递事件队列的互斥锁、连接表的 relaxed 发布、slot 对象的引用计数），因为发行版的 Qt 没有 TSan 插桩。可靠的 TSan 验证需要用 TSan 编译的 Qt。
