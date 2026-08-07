/*
 * EventLoop 最小测试：验证定时器 + fd 事件。
 */
#include "platform/event_loop.h"

#include <cassert>
#include <cstdio>
#include <unistd.h>

int main() {
    auto loop = asternet::platform::create_event_loop();
    assert(loop != nullptr);

    // 1. 定时器测试
    int timer_hit = 0;
    loop->schedule_timer(30, [&] { timer_hit = 1; });
    // 阻塞等待最多 200ms
    for (int i = 0; i < 20 && timer_hit == 0; i++) {
        loop->poll_once(20);
    }
    assert(timer_hit == 1);
    std::printf("[ok] timer fired\n");

    // 2. fd 可读测试（pipe 写一字节，读端应触发）
    int pfd[2];
    assert(pipe(pfd) == 0);
    int fd_hit = 0;
    loop->add_fd(pfd[0], asternet::platform::EventLoop::kReadable, [&](int events) {
        assert(events & asternet::platform::EventLoop::kReadable);
        char buf[8];
        read(pfd[0], buf, 1);
        fd_hit = 1;
    });
    write(pfd[1], "x", 1);
    for (int i = 0; i < 20 && fd_hit == 0; i++) {
        loop->poll_once(20);
    }
    assert(fd_hit == 1);
    std::printf("[ok] fd readable fired\n");

    loop->remove_fd(pfd[0]);
    close(pfd[0]);
    close(pfd[1]);

    std::printf("test_event_loop OK\n");
    return 0;
}
