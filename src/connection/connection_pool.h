/*
 * AsterNet 网络核心 —— 连接管理接口
 *
 * 长短连接统一池：短连接（HTTP/3 stream）与长连接（IM）复用同一 QUIC 连接多路复用。
 * 连接迁移：QUIC Connection ID 与四元组解耦，网络切换时平滑迁移，在途请求不丢。
 * 阶段 2（短连接）/ 阶段 3（长连接）实现。
 */
#ifndef ASTERNET_CONNECTION_POOL_H
#define ASTERNET_CONNECTION_POOL_H

#include <cstdint>
#include <string>

#include "asternet/asternet.h"  // asternet_network_t

namespace asternet {
namespace connection {

class ConnectionPool {
public:
    virtual ~ConnectionPool() = default;

    // 预连接（含 0-RTT）
    virtual int prefetch(const std::string &host) = 0;

    // 触发连接迁移（网络切换时由 Client::on_network_change 调用）
    // 返回 ASTERNET_OK 表示迁移成功，在途请求不丢；失败则上层降级重连 + 幂等重试。
    virtual int migrate(asternet_network_t new_net) = 0;

    // 释放空闲连接（destroy 时调用）
    virtual void evict_all() = 0;
};

}  // namespace connection
}  // namespace asternet

#endif  // ASTERNET_CONNECTION_POOL_H
