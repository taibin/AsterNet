/*
 * AsterNet 网络核心 —— 平台适配：线程与回调投递
 *
 * TaskRunner：把核心回调投递到端侧指定线程（通常主线程），保证回调线程安全。
 *   - Android：token 包装主线程 Looper，投递 Runnable 到主线程。
 *   - iOS：token 为 dispatch_queue_t(main)，dispatch_async 投递。
 * EventLoop：网络线程事件循环（Android/Linux epoll，iOS kqueue），所有 socket I/O 在此。
 * 阶段 1 起实现，token 的具体解释由端侧壳约定。
 */
#ifndef ASTERNET_TASK_RUNNER_H
#define ASTERNET_TASK_RUNNER_H

#include <functional>
#include <memory>

namespace asternet {
namespace platform {

// 投递一个任务到端侧回调线程。token 由 asternet_client_config_t.callback_thread_token 传入。
void post_to_callback_thread(void *token, std::function<void()> task);

// 事件循环接口（网络线程）
class EventLoop {
public:
    virtual ~EventLoop() = default;
    virtual void run() = 0;       // 阻塞运行
    virtual void stop() = 0;
    virtual int  add_fd(int /*fd*/, uint32_t /*events*/) { return 0; }
    virtual int  mod_fd(int /*fd*/, uint32_t /*events*/) { return 0; }
    virtual int  del_fd(int /*fd*/) { return 0; }
};

}  // namespace platform
}  // namespace asternet

#endif  // ASTERNET_TASK_RUNNER_H
