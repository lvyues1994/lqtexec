# lqtexec 设计

## 定位

lqtexec 用 [lexec](https://github.com/lvyues1994/lexec)（C++17 的 `std::execution` 实现）封装 Qt，让 Qt 程序里的异步来源以 sender/receiver 组合：

- 在 Qt 线程上执行：GUI 线程、带事件循环的 `QThread`（scheduler）；
- 等待 Qt 的异步事件：信号、定时器（sender）；
- 取消映射到 Qt 的动作：断开连接、撤下定时器；
- QObject 析构时，挂在它身上的工作被停止，之后的回调不再碰它；程序退出时先停止、再排空，最后退出事件循环。

lqtexec 只做适配，不重复 lexec 的算法：`then`、`let_value`、`when_all`、`when_any`、`counting_scope` 都直接用 lexec 的。

## 已确定的决定

- **Qt 版本**：核心同时支持 Qt 5.15 与 Qt 6，以 Qt 6 为主。核心只用两个版本都有的 API：`QCoreApplication::postEvent`、成员函数指针形式的 `QObject::connect`、`QBasicTimer`。不用 Qt 6 才有的 `Qt::SingleShotConnection`。
- **执行上下文显式创建**：组合根创建 `EventLoopContext` 并把调度器注入使用方，没有按线程查找的全局注册表。这与 lexec 的 `run_loop`、`static_thread_pool` 一致。
- **定时器接口在本地**：`lqtexec::schedule_after` / `schedule_at`，形状与 lrclexec 的 `schedule_after(TimerScheduler, duration)` 一致。lexec 将来有了定时调度器概念再迁移。
- **对象销毁映射为 `set_stopped`**：对应 Qt「上下文对象销毁即断开」的语义。
- **语言与编译器**：C++17，与 lexec 相同：GCC 10+、Clang 12+。
- **异常**：同时支持开启异常和 `-fno-exceptions`。`set_error_t(std::exception_ptr)` 只在开启异常、且操作确实可能抛出时出现（沿用 lexec 的 `eptr_completion_if_t`）。
- **核心依赖**：只有 Qt::Core 和 lexec 的头文件（`lexec::lexec`）。核心不声明 Q_OBJECT 类，使用方不需要为 lqtexec 运行 moc。
- **与 Qt 关键字宏共存**：lexec 包含 `<execution>`；装有 TBB 时，libstdc++ 用 TBB 实现它，而 TBB 的头文件有名为 `emit()` 的成员，会被 Qt 的 `emit` 宏破坏。
  - lqtexec 统一经 `lqtexec/detail/Lexec.h` 引入 lexec，引入期间暂时撤掉 `emit`，所以使用方先包含 Qt 头文件也能编译。
  - 头文件检查对每个公开头文件都验证「先包含 `<QtCore>`」的顺序。
  - 使用方若在 Qt 之后自己直接包含 `<execution>` 或 lexec，仍需自行处理（或定义 `QT_NO_KEYWORDS`）。

## 模块

依赖自上而下，下层不知道上层存在。

1. **`EventLoopContext`（执行资源）**：把 lexec 的侵入式任务模型接到某个线程的 Qt 事件循环上：任务队列、唤醒事件、截止时间堆。不可移动，由组合根持有。`EventLoopThread` 是「`QThread` + `EventLoopContext`」的 RAII 组合。
2. **`EventLoopScheduler`（值句柄）**：`schedule()`、`schedule_after(duration)`、`schedule_at(time_point)`。
3. **`wait_signal`**：把一次信号发射变成一个 sender。
4. **生命周期**：`ObjectScope` 把一个 `counting_scope` 绑到对象的生命周期上；`exec_with_scope` 负责应用收束。

之后的阶段在 2、3 之上加领域适配（network、process、QFuture），各自是独立的 CMake 目标。

## 不变量

lqtexec 的每个操作都满足 lexec 的协议不变量（惰性、恰好完成一次、`noexcept`、地址稳定、完成后不再访问 `this`、签名诚实）。另外：

1. **Qt 回调不碰已完成的操作**：Qt 持有的回调（slot 对象、投递的事件、定时器）在操作完成之后，不再访问操作的内存。
2. **对象线程上的判断**：凡是读取 QObject 状态或对象存活与否的判断，都在该对象所在的线程上做。
3. **不阻塞析构**：QObject 析构时，lqtexec 不开嵌套事件循环，也不阻塞等待。

## EventLoopContext 与 EventLoopScheduler

### 为什么不把 QEvent 嵌进 op state

最直接的零分配做法是：op state 自己包含一个 `QEvent`，start 时 `postEvent`。这条路走不通，原因有两个：

- Qt 在接收者的 `event()` 返回之后才 delete 事件。5.15 和 6.8 的 `sendPostedEvents` 里都是 `QScopedPointer<QEvent> event_deleter(e)`。
- lexec 允许接收方在完成回调里销毁整个 op。于是事件还没被 Qt 删除，它所在的内存就已经释放了。把完成动作挪到事件的析构函数里也不行：派生类的析构体执行完之后还要执行 `~QEvent`。

### 任务队列与唤醒事件

- 每个上下文有一个侵入式 FIFO（互斥锁保护），外加一个住在目标线程的派发 QObject。派发 QObject 只重写 `event()` 和 `timerEvent()`，不需要 moc。
- `schedule()` 的 op state 本身就是队列节点，入队不分配。队列从空变为非空时，才投递一个唤醒事件（`new QEvent`，由 Qt 拥有并删除）。所以分配按批计，不是每个任务一次。
- **派发**：先清掉唤醒标志，记下当前最后一个任务的序号，然后逐个弹出并执行，直到超过这个序号。
  - 执行期间新入队的任务要等下一个唤醒事件，所以大量任务不会饿死绘制和输入事件。
  - 弹出是逐个进行的，队列本身由上下文持有。执行一个任务之前，如果它后面还有任务、而且没有唤醒事件在途，就再投递一个唤醒事件。
  - 这样任务里开了嵌套事件循环（`QDialog::exec()`、`processEvents()` 等）时，嵌套循环会接着执行外层还没执行到的任务，和 Qt 自己的投递事件一样，并保持全局 FIFO。如果外层先取走整批任务，就可能出现嵌套循环等待一个被外层扣住的任务。
  - 代价是含多个任务的批次多一个唤醒事件：每批至多两次分配。
- **停止**：和 lexec 的 `run_loop` 一样，任务执行时检查接收者的 stop token，已请求停止就以 `set_stopped` 完成，不从队列里摘除。
- **为什么用投递事件，不用派发器钩子**：派发器钩子指 `QAbstractEventDispatcher::wakeUp` 配合 `aboutToBlock`。投递事件在嵌套事件循环里会被投递，在平台原生的模态循环（菜单、拖动、原生对话框）里也会；派发器钩子不会。而且线程启动之前，派发器本身还不存在。
- **唤醒事件分配失败**：按 Qt 的惯例视为不可恢复。所以 `schedule()` 的完成签名只有 `set_value_t()` 和 `set_stopped_t()`。

### 定时器

- **结构**：每个上下文一个 `QBasicTimer`（`Qt::PreciseTimer`），挂在最早的截止时间上。定时器操作是侵入式配对堆的节点，不分配；截止时间相同时按启动顺序排序。
- **到期**：到期的定时器移入任务 FIFO，与 `schedule()` 共用同一套派发规则。截止时间在启动时就已经过去的，直接进入 FIFO。
- **跨线程启动**：在其他线程上启动的定时器，在锁内入堆。如果它成了最早的截止时间，就借唤醒事件让派发线程重新挂定时器。`QBasicTimer` 只在派发线程上操作。
- **精度**：Qt 5 的定时器只有毫秒精度。到期时与 `steady_clock` 重新比较，提前触发就重新挂起，保证不早于截止时间完成。
- **停止立即生效**：
  - 典型场景是 `when_any(work, schedule_after(sch, 5s))`：work 先完成时，`when_any` 会停止计时分支并等它结束。如果计时分支要等到截止时间才完成，就要白等 5 秒。
  - 所以停止回调在锁内把节点摘出堆；摘到了，就在请求停止的线程上直接 `set_stopped`。lexec 的 `inplace_stop_source` 允许回调里销毁整个操作，这样做是安全的。
  - 因此定时器 sender 只为 `set_value` 报告完成调度器。
- **启动与停止的竞态**：
  - 先注册停止回调，再入堆。注册期间同步触发的停止，只把节点标记为已取消，入堆时发现已取消就以 `set_stopped` 完成。
  - 这样不会在构造停止回调的过程中完成操作（构造中完成，等于在操作的成员还没构造完时销毁操作）。
  - 节点状态（空闲、在堆中、已到期、已取消）由上下文的锁保护。

### 调度器

`EventLoopScheduler` 是指向上下文的值句柄。按上下文比较相等，`get_forward_progress_guarantee` 报告 `parallel`。

| sender | 完成签名 | 完成调度器 |
| --- | --- | --- |
| `schedule()` | `set_value_t()`、`set_stopped_t()` | value 与 stopped 都是该调度器 |
| `schedule_after(d)` / `schedule_at(t)` | `set_value_t()`、`set_stopped_t()` | 只有 value |

`schedule_after` 的时长从 start 起算。

### 生命周期规则

- 上下文在 `QCoreApplication` 之后创建、之前销毁。
- 只能在它自己的线程上销毁，或者在该线程的事件循环退出之后销毁。不能在它自己派发的任务里销毁它。
- 销毁时如果还有未执行的任务或未到期的定时器，就在销毁线程上以 `set_stopped` 完成它们，并 `qWarning`。这对应 Qt 丢弃未投递事件的语义，同时让 scope 还能 join。
- 线程结束时（`QThread::finished`，在该线程上发出），派发器停掉自己的 `QBasicTimer`，免得之后在别的线程析构时 Qt 报告跨线程停止定时器。

### EventLoopThread

构造时创建并启动一个 `QThread`（`run()` 默认就是事件循环），再在其上建立上下文。析构时先 `quit()`、`wait()`，然后销毁上下文和线程。析构之前，线程上的工作应当已经排空（通常由 scope 保证）。

## wait_signal

```cpp
auto clicked = lqtexec::wait_signal(button, &QPushButton::clicked);  // set_value_t(bool)
```

- **完成签名**：`set_value_t(Args...)`，其中 `Args` 就是信号声明的参数类型（比如 `QString const &`）；另外有 `set_stopped_t()`，开启异常时还有 `set_error_t(std::exception_ptr)`（建立连接可能分配失败）。
  - 私有信号末尾的 `QPrivateSignal` 标签不出现在签名里。它是各个类私有的类型，无法直接比较，所以在编译期从 `__PRETTY_FUNCTION__` 中匹配类型名来识别（与 QCoro 相同的办法）。
- **连接方式**：start 时用 `Qt::DirectConnection` 同时连接目标信号和 `QObject::destroyed`，上下文对象就是该对象本身。
  - connect 是线程安全的，所以 start 可以在任意线程。
  - 直接连接不要求参数类型注册 metatype，也省掉一次拷贝。
- **完成位置**：在发射信号的线程上内联完成，参数按引用转发。需要跨线程时由 `continues_on` 衰变存值。
- **恰好一次**：
  - 竞争的三方是：信号发射、`destroyed`、停止请求。
  - 难点在于，Qt 的 disconnect 不等待另一个线程上正在进行的发射。5.15 的 `doActivate` 无锁遍历连接表，`receiver` 用 relaxed load 读取，调用槽时不持锁，所以 disconnect 返回之后槽仍可能被调用。
  - 因此每次 start 分配一个小的共享控制块，由操作和 Qt 的 slot 对象共同持有。原子状态放在控制块里。
  - 败者只读写控制块，永远不碰操作；胜者独占操作：先断开两个连接、撤下停止回调，再完成。
  - 迟到的槽调用发现已有胜者就返回。控制块随最后一个 slot 对象释放。
- **注册期间**：start 完成注册之前，到达的停止请求和 `destroyed` 只记一个标记，由 start 在结尾完成；这期间的发射被忽略。
  - 契约：start 返回之后的发射一定会被观察到，与 start 并发的发射可能被忽略。
  - 这和边沿触发的语义一致（start 之前的发射本来就会丢）。
- **对象为空、对象销毁、停止请求**：都以 `set_stopped` 完成。
- **分配**：每次 start 有一个控制块，加上 Qt 自己的分配（两条连接记录、两个 slot 对象）。
- **局限**：
  - 边沿触发：start 之前的发射会丢。需要「检查状态再等待」的场景（`QNetworkReply`、`QProcess`），由之后阶段的领域适配在 start 里创建资源来解决。
  - 多次发射的信号不能用 `repeat` 加 `wait_signal`：循环一旦跨线程，两轮之间的发射就会丢；Qt 也不会把正在进行的发射投递给发射期间新建的连接（源码中的 `highestConnectionId`）。这类场景需要只连接一次、带缓冲的 `SignalChannel`，放在后续阶段。

## ObjectScope

```cpp
struct MainWindow : QMainWindow {
    MainWindow(lqtexec::EventLoopScheduler gui, lexec::counting_scope &app)
        : tasks{this, gui, app.get_token()} {}

    void load() {
        lexec::spawn(
            fetchModel()
                | lexec::continues_on(tasks.scheduler())  // 回到 GUI 线程；*this 已析构则 set_stopped
                | lexec::then([this](Model m) { view->setModel(std::move(m)); }),
            tasks.get_token());
    }

    lqtexec::ObjectScope tasks;  // 最后一个成员，最先析构
};
```

- **句柄与状态**：句柄是对象的最后一个成员。真正的状态（`counting_scope`、存活标志、join 操作、引用计数）单独分配一次，由句柄、守护操作和进行中的 join 共同持有。
- **句柄析构**（在对象的线程上，处于 `~Owner` 的成员析构阶段）：把存活标志置为 false，开始收束（`close`、`request_stop`，然后启动 join），随即返回。不阻塞，也不开嵌套循环。
- **守卫调度器**：
  - `tasks.scheduler()` 返回的调度器，任务在对象的线程上执行时先看存活标志；对象已死就以 `set_stopped` 完成。
  - 检查和紧随其后的同步 `then` 在同一次事件派发里执行，对象不可能在两者之间析构。这就是 `QObject::connect(src, sig, context, fn)` 中上下文对象的保证，用在 sender 流水线上。
  - 守卫调度器的操作在 start 时与 scope 建立关联，关联不上（scope 已关闭）就以 `set_stopped` 完成；所以它总在状态的生命期内执行。
- **使用规则**：凡是碰 `this` 的函数，必须紧跟在 hop 到 `tasks.scheduler()` 之后。`lqtexec::then_on(tasks, fn)` 是 `continues_on(tasks.scheduler()) | then(fn)` 的简写。
- **scope 树**：
  - 构造时往父 scope 里 spawn 一个守护操作。父 scope 请求停止时，守护操作开始收束子 scope；子 scope join 之后，守护操作完成。
  - 父 scope 的 join 因此自然等待所有子 scope，只用到 lexec 的公开 API。
  - 父 scope 已关闭时，spawn 不启动守护操作而直接销毁它，这时子 scope 立即关闭，不接受新工作。
- **token**：`tasks.get_token()` 返回的 scope token 包装 sender，使它看到子 scope 的停止请求，环境里的 `get_scheduler` 回答守卫调度器。token 只在句柄存活期间使用，与 lexec `counting_scope` 的 token 一样。
- **收束只启动一次**：由父 scope 的停止或句柄析构中先到的一方启动。join 的接收者不回答 `get_start_scheduler`，所以 join 在最后一个关联结束的线程上直接完成，不依赖事件循环还在运行。

## exec_with_scope

```cpp
int main(int argc, char **argv) {
    auto app = QApplication{argc, argv};
    auto gui = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    auto window = MainWindow{gui.get_scheduler(), scope};
    window.show();
    return lqtexec::exec_with_scope(app, scope, stop);
}
```

- **退出约定**：程序用 `stop.request_stop()` 请求退出，而不是直接 `quit()`。
- **停止回调**（任意线程）：`close`、`request_stop` 应用 scope，再启动 join。join 完成后，以排队调用在主线程上执行 `QCoreApplication::exit()`。必须排队，原因有两个：
  - `exit()` 不是线程安全的（Qt 6.8 文档明确写了）；
  - 在 `exec()` 之前调用 `exit()` 没有效果。
- **直接 `quit()` 的兜底**：有人直接调用了 `quit()` 时，`exec()` 返回后照样收束，再跑一个局部 `QEventLoop` 直到 join 完成。这样可行，是因为 `QCoreApplication::exec()` 在循环结束后会重置 `quitNow`（5.15 与 6.8 都是）。
- **析构顺序**：有 scope 树，`exec_with_scope` 返回时所有窗口的工作都已排空，main 末尾析构窗口时 `ObjectScope` 的析构是空操作。
- **核心只依赖 Qt::Core**：所以 `QGuiApplication::lastWindowClosed` 到 `request_stop` 的接线由应用自己完成。

## 分配

| 位置 | 分配 |
| --- | --- |
| `schedule()`、定时器 | 操作本身不分配；每批任务至多两个唤醒事件（入队时一个，派发时为剩余任务一个） |
| `wait_signal` | 每次 start 一个控制块，加上 Qt 的连接记录与 slot 对象 |
| `ObjectScope` | 每个对象一次状态，加上守护操作的 spawn |
| `exec_with_scope` | 收束时一次排队调用 |

## 放弃的方案

- **每次 hop 用 `QMetaObject::invokeMethod(ctx, fn)`**：每次分配两次（slot 对象和 `QMetaCallEvent`），不能批处理，上下文关闭时也无从收拾。Qt 官方的实验仓库 [libunifex-with-qt](https://git.qt.io/vivoutil/libunifex-with-qt) 就是这样做的。
- **QEvent 嵌进 op state**：见上，与「完成时可销毁 op」冲突。
- **派发器钩子**（`wakeUp` + `aboutToBlock`）：在原生模态循环里不投递，线程启动前也不可用。
- **用 lexec 的 `run_loop` 替换 Qt 的事件派发器**：要覆盖 glib、Cocoa、Win32 和各平台插件，侵入太大。
- **以 QFuture 为中心桥接**：Qt 5 没有 `then`，每一步都要分配共享状态，也没有结构化取消。
- **在 QObject 析构函数里阻塞 join**：析构中途开嵌套事件循环会执行任意事件，包括删除别的对象甚至自己。
- **用 `destroyed` 信号或子对象触发 ObjectScope 收束**：`destroyed` 在 `~QObject` 里发出，此时派生类的成员都已析构；作为最后一个成员的句柄析构得更早，也不依赖 Qt。

## 与参考实现的关系

- **Qt 官方原型（`qtstdexec.h`）**：保留它的 API 手感（以对象和成员函数指针等待信号）。它的问题正是本设计要解决的：
  - 排队执行的 lambda 按引用捕获了信号参数；
  - 停止回调在执行中 reset 自己，之后又经已销毁对象的 `this` 访问接收者；
  - disconnect 与跨线程发射的竞态没有处理；
  - 对象销毁不处理，操作永远挂起；
  - 在析构里开嵌套事件循环。
- **lrclexec**：沿用工程约定：命名（PascalCase 的类型与头文件，snake_case 的 sender 工厂）、目录结构、lexec 的提供方式。不沿用「每个操作一个 `shared_ptr` 状态 + `std::function` 投递」：Qt 的事件投递和对象亲和都在我们手里，op state 可以像 lexec 那样内嵌。

## 命名与风格

- 类型与头文件用 PascalCase，成员函数用 camelCase；lexec 协议要求的名字（`schedule`、`connect`、`start`、`set_value`、`get_env`、`get_scheduler`、`get_token` 等）与 sender 工厂（`wait_signal`、`schedule_after`、`exec_with_scope`）用 snake_case。
- 用 `struct` 关键字、east const、严格的 `noexcept` 纪律。不写裸 `new`，例外只有两处：交给 Qt 拥有的唤醒事件，以及 `ObjectScope` 状态的工厂（与引用计数归零时的 `delete this` 成对，封装在引用类型里）。

## 工程结构

```
lqtexec/
  CMakeLists.txt  CMakePresets.json
  cmake/          仅用于自身开发构建的选项
  lqtexec/        核心：lqtexec::lqtexec（STATIC，Qt::Core + lexec::lexec）
    include/lqtexec/  EventLoopContext.h EventLoopScheduler.h EventLoopThread.h WaitSignal.h
                      ObjectScope.h ExecWithScope.h detail/...
    src/
  tests/          doctest；测试入口先建立 QCoreApplication
  examples/
  docs/
```

- **lexec 的提供方**：父项目已有 `lexec::lexec` 时直接使用；否则用 `LQTEXEC_LEXEC_SOURCE_DIR` 指向本地源码；都没有时，用 FetchContent 拉取固定的提交。
- **Qt 版本选择**：由 `LQTEXEC_QT_MAJOR_VERSION` 决定（5 或 6），留空时优先 Qt 6。
- **Presets**：GCC 与 Clang 的 debug、asan、tsan，GCC 的 release 与 noexcept，以及 Qt 5 的 `gcc-debug-qt5`、`gcc-noexcept-qt5`、`clang-asan-qt5`。

## 实施计划

**阶段 0：工程骨架**

- 内容：CMake 目标、presets、lexec 提供方、Qt 5/6 选择、doctest、建立 `QCoreApplication` 的测试入口、带超时并驱动事件循环的测试辅助、CI。
- 验收：Qt 5.15 与 Qt 6 下构建通过，头文件自包含检查通过。

**阶段 1：执行资源**

- 内容：`EventLoopContext`、`EventLoopScheduler`、定时器、`EventLoopThread`。
- 验收：
  - 完成发生在上下文的线程上（GUI 线程与工作线程）；
  - FIFO；
  - 派发期间新入队的任务排在其间投递的 Qt 事件之后；
  - 嵌套事件循环里执行外层未执行的任务；
  - 启动一批任务只分配一个唤醒事件；
  - 停止请求；
  - 定时器不早于截止时间，相同截止时间按启动顺序；
  - `when_any` 中的定时器被停止时立即完成；
  - 跨线程启动与停止；
  - 带未完成工作销毁上下文。

**阶段 2：信号与生命周期**

- 内容：`wait_signal`、`ObjectScope`、`then_on`、`exec_with_scope`。
- 验收：
  - 参数类型与私有信号的签名；
  - 对象销毁、停止、空对象；
  - 多线程同时发射与「发射对停止」的压力测试中恰好完成一次，ASan 下无越界访问；
  - 守卫调度器在对象析构后以 `set_stopped` 完成；
  - 句柄析构不阻塞，状态在 join 后释放；
  - 父 scope 停止时子 scope 收束且父 join 完成；
  - 父 scope 已关闭时子 scope 不接受工作；
  - `exec_with_scope` 的停止驱动退出、直接 `quit()` 兜底、`exec()` 前已请求停止。

**之后**：`ThreadPoolScheduler`（op state 即 `QRunnable`，`setAutoDelete(false)`，`tryTake` 取消；5.15 与 6.8 都在 `run()` 之前读取 `autoDelete()`）、network 与 process 适配、`SignalChannel`、QFuture 互操作（先 Qt 6）、Unix 信号到停止的桥接、安装导出。

## 风险

- **TSan**：发行版的 Qt 不是用 TSan 编译的，`QMutex` 直接基于 futex，TSan 在跨线程 Qt 代码上会误报和漏报。
  - 实测（Qt 6.8.3、Clang 18）：测试全部通过，但有 16 条报告，全部来自没有插桩的同步，没有一条落在 lqtexec 自己的同步上。具体是：
    - 唤醒事件经 Qt 投递队列的互斥锁传递；
    - 新连接经 Qt 连接表的 relaxed 原子操作发布给发射线程；
    - slot 对象的引用计数与孤儿连接清理，都在 libQt6Core 里；
    - glib 的 eventfd。
  - 不加模块级抑制，因为那样会连带屏蔽被 Qt 回调的 lqtexec 代码。跨线程的正确性主要靠压力测试加 ASan；需要时用 TSan 编译的 Qt 复核。
- **私有信号的识别**依赖编译器 `__PRETTY_FUNCTION__` 的格式，由静态断言测试覆盖 GCC 与 Clang。
- **Qt 内部行为**：本设计依赖的几处 Qt 行为（事件删除时机、`doActivate` 的无锁遍历、`exec()` 后重置 `quitNow`、`QThread::finished` 在派发器删除前发出），已对照 qtbase 5.15 与 6.8 的源码核实，并由测试固定。
