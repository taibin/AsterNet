/*
 * AsterNet 网络核心 —— 智能 DNS 接口
 *
 * 解析链路：受信任的 HttpDNS（可选）→ LocalDNS → 短期 Backup IP → 内存缓存。
 * 解析结果始终保留逻辑域名，调用方只能把 IP 用作拨号地址，不能替换 TLS SNI 或
 * 证书主机名。端侧会根据真实建连结果更新地址排序，不发起无预算的主动探测。
 */
#ifndef ASTERNET_DNS_RESOLVER_H
#define ASTERNET_DNS_RESOLVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asternet/asternet.h"
#include "sdt/quality_prober.h"

namespace asternet {
namespace dns {

enum class ResolutionSource {
    kHttpDns,
    kLocalDns,
    kBackup,
    kLiteral,
};

// 单个解析结果 IP。score 越高越优；未知 RTT/丢包不会被当作优质链路。
struct IpResult {
    std::string ip;
    int rtt_ms = -1;
    int loss_rate = -1;  // 千分比，-1 表示未测
    int score = 0;
    bool ipv6 = false;
    ResolutionSource source = ResolutionSource::kLocalDns;
};

struct ResolveResult {
    std::vector<IpResult> addresses;
    int error = ASTERNET_OK;
    int64_t elapsed_ms = 0;
    bool cache_hit = false;
};

class SmartDnsResolver {
public:
    virtual ~SmartDnsResolver() = default;

    // 同步解析：受调用方 deadline 约束。结果已按 score 降序排列。
    virtual std::vector<IpResult> resolve(const std::string &host) = 0;

    virtual ResolveResult resolve_with_metadata(const std::string &host,
                                                uint64_t /*network_epoch*/,
                                                int /*timeout_ms*/ = 0) {
        ResolveResult result;
        result.addresses = resolve(host);
        result.error = result.addresses.empty() ? ASTERNET_ERR_DNS : ASTERNET_OK;
        return result;
    }

    // 预解析：启动/切前台时对关键域名提前解析并探测，结果入缓存
    virtual int prefetch(const std::string &host) = 0;
    virtual int prefetch(const std::string &host, uint64_t network_epoch) {
        (void)network_epoch;
        return prefetch(host);
    }

    // 清除指定 host 缓存（host 为空则全清）
    virtual void invalidate(const std::string &host = "") = 0;

    // 来自真实请求的地址质量反馈。主动 TCP/UDP 探速容易放大弱网流量，因此不作为默认策略。
    virtual void report_connection_result(const std::string & /*host*/, const std::string & /*ip*/,
                                          uint64_t /*network_epoch*/, bool /*success*/,
                                          int /*rtt_ms*/) {}

    virtual void on_quality_change(const sdt::QualitySnapshot & /*snapshot*/) {}

    virtual void on_network_change(uint64_t /*network_epoch*/) {}
    virtual std::string dump() const { return "{}"; }
};

class SmartDnsResolverImpl final : public SmartDnsResolver {
public:
    struct Config {
        int64_t ttl_ms = 300000;
        int64_t stale_ttl_ms = 60000;
        size_t max_cache_entries = 128;
        size_t max_health_entries = 512;
        size_t max_active_lookups = 4;
        int default_lookup_timeout_ms = 3000;
        bool allow_private_addresses = false;
    };

    // HttpDNS 的可信传输和 bootstrap 由平台/业务层注入。没有可信 provider 时，
    // 默认只使用 LocalDNS，不内置硬编码 VIP 或公共服务地址。
    using HttpDnsLookup = std::function<std::vector<IpResult>(const std::string &host)>;

    SmartDnsResolverImpl();
    explicit SmartDnsResolverImpl(Config config, HttpDnsLookup httpdns_lookup = {});
    ~SmartDnsResolverImpl() override;

    std::vector<IpResult> resolve(const std::string &host) override;
    ResolveResult resolve_with_metadata(const std::string &host,
                                        uint64_t network_epoch,
                                        int timeout_ms = 0) override;
    int prefetch(const std::string &host) override;
    int prefetch(const std::string &host, uint64_t network_epoch) override;
    void invalidate(const std::string &host = "") override;
    void report_connection_result(const std::string &host, const std::string &ip,
                                  uint64_t network_epoch, bool success, int rtt_ms) override;
    void on_quality_change(const sdt::QualitySnapshot &snapshot) override;
    void on_network_change(uint64_t network_epoch) override;
    std::string dump() const override;

    void set_httpdns_lookup(HttpDnsLookup httpdns_lookup);
    void set_backup_ips(const std::string &host, std::vector<IpResult> addresses);

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace dns
}  // namespace asternet

#endif  // ASTERNET_DNS_RESOLVER_H
