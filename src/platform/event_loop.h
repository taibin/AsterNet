/*
 * AsterNet 网络核心 —— 跨平台事件循环
 *
 * QuicEngine 的事件驱动基础：socket 可读/可写事件 + oneshot 定时器。
 * - macOS / iOS：kqueue（EVFILT_READ/WRITE + EVFILT_TIMER）
 * - Android / Linux：epoll（EPOLLIN/OUT + timerfd）
 *
 * 线程模型：EventLoop 不 owns 线程，由调用方在专用网络线程 run()。
 * 所有回调在该网络线程触发，端侧回调再经 TaskRunner 抛回主线程。
 */
#ifndef ASTERNET_EVENT_LOOP_H
#define ASTERNET_EVENT_LOOP_H

#include <cstdint>
#include <functional>
#include <memory>

namespace asternet {
namespace platform {

class EventLoop {
public:
    enum Event : int {
        kReadable = 0x1,
        kWritable = 0x2,
    };

    using Callback = std::function<void(int events)>;
    using TimerCallback = std::function<void()>;

    virtual ~EventLoop() = default;

    // 注册 fd 事件（持续，直到 remove_fd）。cb 在网络线程触发。
    virtual void add_fd(int fd, int events, Callback cb) = 0;
    virtual void mod_fd(int fd, int events) = 0;
    virtual void remove_fd(int fd) = 0;

    // oneshot 定时器：delay_ms 后触发 cb 一次。可重复调用以 reschedule。
    // QuicEngine 用此实现 xquic 的 set_event_timer 回调。
    virtual void schedule_timer(uint64_t delay_ms, TimerCallback cb) = 0;

    // 阻塞运行事件循环，直到 stop()。
    virtual void run() = 0;
    virtual void stop() = 0;

    // 跑一轮（测试/集成用），timeout_ms < 0 表示阻塞等待，0 表示非阻塞，>0 表示最多等待。
    // 返回是否处理了事件。
    virtual bool poll_once(int timeout_ms) = 0;
};

// 工厂：按平台创建 kqueue / epoll 实现。
std::unique_ptr<EventLoop> create_event_loop();

}  // namespace platform
}  // namespace asternet

#endif  // ASTERNET_EVENT_LOOP_H
