/*
 * AsterNet 网络核心 —— 逐跳路由追踪（traceroute）
 *
 * 采用 Linux/Android 无特权方案：普通 UDP socket + ICMP 错误队列（IP_RECVERR +
 * recvmsg(MSG_ERRQUEUE)），无需 raw socket / root。逐跳递增 TTL，靠沿途路由器返回的
 * ICMP Time Exceeded 得到中间跳地址与 RTT，靠目标端口未监听返回的 ICMP Port Unreachable
 * 判定到达目标。
 *
 * 局限：部分网络过滤 ICMP/UDP 时个别跳会超时（rtt 记为 -1）；当前仅实现 IPv4。
 */
#ifndef ASTERNET_TRACEROUTE_H
#define ASTERNET_TRACEROUTE_H

#include <cstdint>
#include <string>
#include <vector>

namespace asternet {
namespace sdt {

struct TracerouteConfig {
    int max_hops = 30;          // 最大跳数
    int probes_per_hop = 3;     // 每跳探测次数（经典 traceroute 为 3）
    int probe_timeout_ms = 2000; // 单次探测超时
};

struct TracerouteHop {
    int ttl = 0;
    std::string addr;                 // 该跳 IP；全程无响应时为空
    std::vector<double> rtt_ms;       // 每发一次一个值；-1 表示超时
    int loss = 0;                     // 超时探测数
    bool reached = false;             // 是否到达目标
};

struct TracerouteResult {
    std::string host;
    uint16_t port = 0;
    std::string resolved_ip;
    int ip_version = 4;
    int max_hops = 0;
    int probes_per_hop = 0;
    std::string status;               // "reached" | "completed" | "error"
    std::string error;                // 非空仅当 status == "error"
    std::vector<TracerouteHop> hops;
};

// 执行一次同步逐跳探测。host 为空或 port 为 0 时返回 status=="error"。
TracerouteResult trace_route(const std::string &host, uint16_t port,
                             const TracerouteConfig &config = {});

// 将结果序列化为 JSON 字符串（供 C ABI / JNI 返回）。
std::string trace_route_json(const std::string &host, uint16_t port,
                             const TracerouteConfig &config = {});

}  // namespace sdt
}  // namespace asternet

#endif  // ASTERNET_TRACEROUTE_H
