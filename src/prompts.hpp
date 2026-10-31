#pragma once

// Dimension 3 prompt. The Python harness asked for an asyncio scheduler and
// exec'd it. This port asks for the same scheduling contract in C++17 so the
// process can compile and run the answer with g++ and no Python runtime.
// Scores are not comparable to the old Python sandbox.

inline const char* prompt_code() {
    return R"PS5CODE(请使用 C++17（仅限标准库，严禁第三方库，不要使用 C++20 协程）实现一个生产级的优先级动态令牌桶任务调度器类 `PriorityScheduler`。评测器会用 `g++ -std=c++17 -pthread` 编译你的代码，并链接一段隐藏测试驱动。类必须定义在全局命名空间。请把完整实现放在一个 ```cpp 代码块中，包含必要的标准库头文件，不要省略实现，不要定义 main。

1. 构造函数：
   `PriorityScheduler(double capacity, double refill_rate, int max_concurrency, double starvation_timeout)`
   - 令牌桶初始令牌数等于 `capacity`，之后以 `refill_rate`（令牌/秒）连续补充，且任何时候不超过 `capacity`。
   - `max_concurrency` 是同时处于执行状态的任务上限。
   - `starvation_timeout` 单位为秒。

2. 提交接口（阻塞直到该任务函数返回；失败时抛出异常）：
   `void submit(const std::function<void()>& task, double cost, int priority, double timeout_sec = -1.0)`
   - `cost` 只在任务真正开始执行时一次性扣除。任务还在排队时绝不能扣除令牌。
   - `priority` 数值越小优先级越高（1 高于 2，2 高于 10）。
   - `timeout_sec < 0` 表示不设超时。
   - 若任务在开始执行前，排队等待时间达到 `timeout_sec` 秒，必须抛出 `std::runtime_error`，并且不得扣除令牌。
   - 若任务已经开始执行，执行时长达到 `timeout_sec` 秒时，必须释放其所占的并发槽位，并抛出 `std::runtime_error`。

3. 防饥饿老化（Aging）：低优先级任务在队列中的等待时间超过 `starvation_timeout` 秒后，自动提权为最高优先级，高于任何尚未老化的任务。多个已老化任务之间，以及其余同优先级任务之间，按提交先后 FIFO。未老化时不得让较低优先级（数值更大）的任务插到更高优先级任务前面。

4. 优雅关闭：
   `void shutdown(bool wait = true)`
   - `wait == true` 时等待已排队任务和执行中任务结束。
   - `wait == false` 时取消尚未开始的排队任务（以异常结束这些 submit）。
   - 析构函数必须安全关闭调度器；重复调用 shutdown 不得崩溃。

隐藏测试会从多个线程并发调用 submit，并记录每个 task 开始执行的顺序。请保证上述规则在 `max_concurrency == 1` 时仍然成立。)PS5CODE";
}
