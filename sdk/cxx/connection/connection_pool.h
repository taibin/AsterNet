/*
 * AsterNet 网络核心 —— 连接管理接口
 *
 * 连接管理统一维护 origin、协议、网络代际和租约状态。传输引擎只有在实际提供
 * 持久连接能力时才能将租约标记为复用；本模块不会把元数据命中伪装成连接复用。
 */
#ifndef ASTERNET_CONNECTION_POOL_H
#define ASTERNET_CONNECTION_POOL_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/engine.h"

#include "asternet/asternet.h"  // asternet_network_t

namespace asternet {
namespace connection {

struct Origin {
    std::string host;
    uint16_t port = 443;
    asternet_protocol_t protocol = ASTERNET_PROTOCOL_UNKNOWN;
    uint64_t network_epoch = 0;

    bool operator==(const Origin &other) const {
        return host == other.host && port == other.port && protocol == other.protocol
            && network_epoch == other.network_epoch;
    }
};

struct ConnectionLease {
    Origin origin;
    uint64_t id = 0;
    bool reused = false;
    bool valid = false;
};

struct PoolSnapshot {
    size_t origins = 0;
    size_t active_leases = 0;
    size_t prefetches = 0;
    size_t migrations = 0;
    size_t evictions = 0;
    int last_prefetch_result = ASTERNET_ERR_UNSUPPORTED;
    int last_migration_result = ASTERNET_ERR_UNSUPPORTED;
    uint64_t network_epoch = 0;
};

class ConnectionPool {
public:
    virtual ~ConnectionPool() = default;

    // 预连接请求。只有底层引擎提供持久连接时才会真正建立连接。
    virtual int prefetch(const std::string &host) = 0;

    // 触发网络代际切换。H1/H2 的旧连接将不可复用；H3 是否可迁移取决于底层实现。
    virtual int migrate(asternet_network_t new_net) = 0;

    // 释放空闲连接（destroy 时调用）
    virtual void evict_all() = 0;

    virtual ConnectionLease acquire(const Origin & /*origin*/) { return {}; }
    virtual void release(const ConnectionLease & /*lease*/, bool /*success*/) {}
    virtual void on_network_change(uint64_t /*network_epoch*/, asternet_network_t /*new_net*/) {}
    virtual PoolSnapshot snapshot() const { return {}; }
    virtual std::string dump() const { return "{}"; }
};

class ConnectionPoolImpl final : public ConnectionPool {
public:
    using PrefetchHandler = std::function<int(const std::string &host)>;
    using MigrationHandler = std::function<int()>;

    explicit ConnectionPoolImpl(size_t max_origins = 128);
    ~ConnectionPoolImpl() override;

    int prefetch(const std::string &host) override;
    int migrate(asternet_network_t new_net) override;
    void evict_all() override;
    ConnectionLease acquire(const Origin &origin) override;
    void release(const ConnectionLease &lease, bool success) override;
    void on_network_change(uint64_t network_epoch, asternet_network_t new_net) override;
    PoolSnapshot snapshot() const override;
    std::string dump() const override;

    void set_prefetch_handler(PrefetchHandler handler);
    void set_migration_handler(MigrationHandler handler);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace connection
}  // namespace asternet

#endif  // ASTERNET_CONNECTION_POOL_H
