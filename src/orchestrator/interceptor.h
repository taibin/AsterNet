/*
 * AsterNet 网络核心 —— 请求编排接口
 *
 * 拦截器链：header / base-url / cache / log / logout / retry 等，按序处理请求与响应。
 * 重试/降级调度：host 级熔断器，QUIC 连续失败降级 HTTP/2，再降级 HTTP/1.1。
 * 请求合并：相同 in-flight 请求去重，并发不重复打网。
 * 阶段 1 起实现。
 */
#ifndef ASTERNET_INTERCEPTOR_H
#define ASTERNET_INTERCEPTOR_H

#include <memory>
#include <vector>

namespace asternet {
namespace orchestrator {

// 拦截器接口（链式）。intercept 调用 chain->proceed 推进下一环。
class Interceptor {
public:
    virtual ~Interceptor() = default;
    // virtual Response intercept(Chain &chain) = 0;  // 阶段 1 定义 Request/Response 类型
};

class InterceptorChain {
public:
    virtual ~InterceptorChain() = default;
    // virtual Response proceed(const Request &req) = 0;
    void add(std::unique_ptr<Interceptor> /*i*/) {
        // interceptors_.push_back(std::move(i));  // 阶段 1 启用
    }

private:
    std::vector<std::unique_ptr<Interceptor>> interceptors_;
};

}  // namespace orchestrator
}  // namespace asternet

#endif  // ASTERNET_INTERCEPTOR_H
