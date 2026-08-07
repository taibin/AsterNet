/*
 * AsterNet 网络核心 —— 跨平台事件循环实现
 * - __APPLE__ (macOS/iOS)：kqueue
 * - __linux__ (Android/Linux)：epoll + timerfd
 *
 * 线程安全：非线程安全，约定由单一网络线程驱动。跨线程 stop() 通过原子标志 + 短轮询唤醒。
 */
#include "platform/event_loop.h"

#include <atomic>
#include <cstring>
#include <unordered_map>

#if defined(__APPLE__)
#  include <sys/event.h>
#  include <sys/time.h>
#  include <unistd.h>
#elif defined(__linux__)
#  include <sys/epoll.h>
#  include <sys/timerfd.h>
#  include <unistd.h>
#else
#  error "EventLoop: unsupported platform"
#endif

namespace asternet {
namespace platform {

#if defined(__APPLE__)

/* ---------------- kqueue 实现 (macOS / iOS) ---------------- */

class KqueueEventLoop : public EventLoop {
public:
    KqueueEventLoop() : kq_(kqueue()) {}

    ~KqueueEventLoop() override {
        if (kq_ >= 0) close(kq_);
    }

    void add_fd(int fd, int events, Callback cb) override {
        fd_cbs_[fd] = std::move(cb);
        struct kevent kevs[2];
        int n = 0;
        if (events & kReadable) {
            EV_SET(&kevs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, reinterpret_cast<void *>(fd));
        }
        if (events & kWritable) {
            EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, reinterpret_cast<void *>(fd));
        }
        if (n > 0) kevent(kq_, kevs, n, nullptr, 0, nullptr);
    }

    void mod_fd(int fd, int events) override {
        struct kevent kevs[2];
        int n = 0;
        // 先删除旧的，再加新的（kqueue 无 EPOLL_CTL_MOD 等价，需显式删/加）
        EV_SET(&kevs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&kevs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kq_, kevs, n, nullptr, 0, nullptr);  // 忽略不存在错误
        add_fd(fd, events, fd_cbs_[fd]);
    }

    void remove_fd(int fd) override {
        struct kevent kevs[2];
        EV_SET(&kevs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&kevs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kq_, kevs, 2, nullptr, 0, nullptr);
        fd_cbs_.erase(fd);
    }

    void schedule_timer(uint64_t delay_ms, TimerCallback cb) override {
        timer_cb_ = std::move(cb);
        struct kevent kev;
        // TIMER_IDENT 固定为 1（xquic 单定时器场景）
        EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, delay_ms, nullptr);
        kevent(kq_, &kev, 1, nullptr, 0, nullptr);
    }

    bool poll_once(int timeout_ms) override {
        struct timespec ts;
        struct timespec *pts = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
            pts = &ts;
        }
        struct kevent events[16];
        int n = kevent(kq_, nullptr, 0, events, 16, pts);
        if (n <= 0) return false;
        for (int i = 0; i < n; i++) {
            if (events[i].filter == EVFILT_TIMER) {
                if (timer_cb_) {
                    TimerCallback cb;
                    cb.swap(timer_cb_);
                    cb();
                }
            } else {
                int fd = static_cast<int>(reinterpret_cast<intptr_t>(events[i].udata));
                int ready_events = 0;
                if (events[i].filter == EVFILT_READ) ready_events |= kReadable;
                if (events[i].filter == EVFILT_WRITE) ready_events |= kWritable;
                auto it = fd_cbs_.find(fd);
                if (it != fd_cbs_.end() && it->second) it->second(ready_events);
            }
        }
        return true;
    }

    void run() override {
        running_.store(true);
        while (running_.load()) {
            poll_once(100);  // 100ms 唤醒粒度，检查 running_
        }
    }

    void stop() override { running_.store(false); }

private:
    int kq_;
    std::unordered_map<int, Callback> fd_cbs_;
    TimerCallback timer_cb_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<EventLoop> create_event_loop() {
    return std::make_unique<KqueueEventLoop>();
}

#elif defined(__linux__)

/* ---------------- epoll + timerfd 实现 (Android / Linux) ---------------- */

class EpollEventLoop : public EventLoop {
public:
    EpollEventLoop() : epfd_(epoll_create1(0)) {
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd_ >= 0) {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = timer_fd_;
            epoll_ctl(epfd_, EPOLL_CTL_ADD, timer_fd_, &ev);
        }
    }

    ~EpollEventLoop() override {
        if (epfd_ >= 0) close(epfd_);
        if (timer_fd_ >= 0) close(timer_fd_);
    }

    void add_fd(int fd, int events, Callback cb) override {
        cbs_[fd] = std::move(cb);
        uint32_t ev = 0;
        if (events & kReadable) ev |= EPOLLIN;
        if (events & kWritable) ev |= EPOLLOUT;
        struct epoll_event event{};
        event.events = ev;
        event.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event);
    }

    void mod_fd(int fd, int events) override {
        uint32_t ev = 0;
        if (events & kReadable) ev |= EPOLLIN;
        if (events & kWritable) ev |= EPOLLOUT;
        struct epoll_event event{};
        event.events = ev;
        event.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
    }

    void remove_fd(int fd) override {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        cbs_.erase(fd);
    }

    void schedule_timer(uint64_t delay_ms, TimerCallback cb) override {
        timer_cb_ = std::move(cb);
        struct itimerspec its{};
        its.it_value.tv_sec = delay_ms / 1000;
        its.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;
        timerfd_settime(timer_fd_, 0, &its, nullptr);
    }

    bool poll_once(int timeout_ms) override {
        struct epoll_event events[16];
        int n = epoll_wait(epfd_, events, 16, timeout_ms);
        if (n <= 0) return false;
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == timer_fd_) {
                uint64_t exp;
                read(timer_fd_, &exp, sizeof(exp));  // 清除定时器
                if (timer_cb_) {
                    TimerCallback cb;
                    cb.swap(timer_cb_);
                    cb();
                }
            } else {
                int ready_events = 0;
                if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) ready_events |= kReadable;
                if (events[i].events & EPOLLOUT) ready_events |= kWritable;
                auto it = cbs_.find(fd);
                if (it != cbs_.end() && it->second) it->second(ready_events);
            }
        }
        return true;
    }

    void run() override {
        running_.store(true);
        while (running_.load()) {
            poll_once(100);
        }
    }

    void stop() override { running_.store(false); }

private:
    int epfd_;
    int timer_fd_;
    std::unordered_map<int, Callback> cbs_;
    TimerCallback timer_cb_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<EventLoop> create_event_loop() {
    return std::make_unique<EpollEventLoop>();
}

#endif

}  // namespace platform
}  // namespace asternet
