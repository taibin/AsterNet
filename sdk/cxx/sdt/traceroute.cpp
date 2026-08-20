/*
 * AsterNet 网络核心 —— 逐跳路由追踪（traceroute）实现
 *
 * 采用 Linux/Android 无特权方案：普通 UDP socket + ICMP 错误队列。逐跳递增 TTL，
 * 沿途路由器返回 ICMP Time Exceeded（中间跳），目标端口未监听返回 ICMP Port
 * Unreachable（到达）。全程不需要 raw socket / root。
 */
#include "sdt/traceroute.h"

#include <sstream>
#include <iomanip>

namespace asternet {
namespace sdt {
namespace {

// JSON 字符串转义：转义引号、反斜杠与控制字符，供 Linux 实现与兜底实现共用。
std::string json_escape(const std::string &value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

}  // namespace
}  // namespace sdt
}  // namespace asternet

#if defined(ASTERNET_PLATFORM_LINUX) || defined(ASTERNET_PLATFORM_ANDROID)

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/errqueue.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace asternet {
namespace sdt {

namespace {

#ifndef IP_RECVERR
#define IP_RECVERR 75  // Linux 稳定值，个别 libc 头未导出时的兜底
#endif

double monotonic_seconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 负数（超时）输出 -1；正数输出最多两位小数并去掉尾零。
std::string rtt_to_string(double ms) {
    if (ms < 0) return "-1";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << ms;
    std::string s = oss.str();
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string ipv4_to_string(const struct sockaddr *sa) {
    if (sa == nullptr) return "";
    char buf[INET_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET) {
        const auto *sin = reinterpret_cast<const struct sockaddr_in *>(sa);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr) return "";
    }
    return buf;
}

TracerouteResult trace_route_impl(const std::string &host, uint16_t port,
                                  const TracerouteConfig &config) {
    TracerouteResult result;
    result.host = host;
    result.port = port;
    result.max_hops = config.max_hops;
    result.probes_per_hop = config.probes_per_hop;
    // 单次探测超时上限 60s：异常配置（<=0 或过大）回落为 2000ms，避免 poll 超时整数溢出。
    const int probe_timeout_ms =
        (config.probe_timeout_ms <= 0 || config.probe_timeout_ms > 60000)
            ? 2000
            : config.probe_timeout_ms;

    struct addrinfo hints{};
    hints.ai_family = AF_INET;      // 本次仅 IPv4
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
        result.status = "error";
        result.error = "dns resolve failed";
        return result;
    }
    struct sockaddr_in dest = *reinterpret_cast<const struct sockaddr_in *>(res->ai_addr);
    result.resolved_ip = ipv4_to_string(res->ai_addr);
    result.ip_version = 4;
    freeaddrinfo(res);

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        result.status = "error";
        result.error = "socket create failed";
        return result;
    }

    int on = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_RECVERR, &on, sizeof(on)) < 0) {
        close(fd);
        result.status = "error";
        result.error = "IP_RECVERR unsupported";
        return result;
    }

    // 关键：先 connect 到目标（UDP 软连接，不握手）。
    // 1) 触发 bionic/netdClient 用默认网络的 netId 标记 socket（fwmark），否则未连接的
    //    fwmark=0 socket 在 Android 策略路由下可能落到 unreachable 表 → ENETUNREACH；
    // 2) connected socket 才能可靠地从错误队列收到 ICMP 错误。
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0) {
        const int saved_errno = errno;
        close(fd);
        result.status = "error";
        result.error = (saved_errno == ENETUNREACH || saved_errno == EHOSTUNREACH)
                           ? "no route to host"
                           : "connect failed";
        return result;
    }

    // 已连接，直接 send；发送内容只需非空即可。
    const char payload[] = "asternet-trace";
    bool reached = false;
    bool done = false;  // 收到目的不可达（含非端口）即终止整条追踪，避免后续跳重复报 !H/!N

    for (int ttl = 1; ttl <= config.max_hops && !reached && !done; ++ttl) {
        TracerouteHop hop;
        hop.ttl = ttl;
        if (setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) break;

        for (int probe = 0; probe < config.probes_per_hop && !reached && !done; ++probe) {
            const double send_time = monotonic_seconds();
            const ssize_t sent = send(fd, payload, sizeof(payload) - 1, 0);
            if (sent < 0) {
                if (errno == ENETUNREACH || errno == EHOSTUNREACH) {
                    hop.rtt_ms.push_back(-1);
                    ++hop.loss;
                    result.hops.push_back(hop);
                    close(fd);
                    result.status = "error";
                    result.error = "no route to host";
                    return result;
                }
                hop.rtt_ms.push_back(-1);
                ++hop.loss;
                continue;
            }

            double rtt_ms = -1;
            bool replied = false;
            bool is_reached = false;
            std::string hop_addr;
            const double deadline = send_time + probe_timeout_ms / 1000.0;

            while (true) {
                const double remaining = deadline - monotonic_seconds();
                if (remaining <= 0) break;

                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN | POLLERR;
                const int pr = poll(&pfd, 1, static_cast<int>(remaining * 1000.0) + 1);
                if (pr <= 0) break;  // 超时或出错

                struct sockaddr_storage from{};
                char control[1024];
                char buf[256];
                struct iovec iov{};
                iov.iov_base = buf;
                iov.iov_len = sizeof(buf);
                struct msghdr msg{};
                std::memset(&msg, 0, sizeof(msg));
                msg.msg_name = &from;
                msg.msg_namelen = sizeof(from);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = control;
                msg.msg_controllen = sizeof(control);

                const ssize_t n = recvmsg(fd, &msg, MSG_ERRQUEUE);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    break;
                }

                const struct sock_extended_err *ee = nullptr;
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
                     cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_RECVERR) {
                        ee = reinterpret_cast<const struct sock_extended_err *>(CMSG_DATA(cm));
                        break;
                    }
                }
                if (ee == nullptr) continue;  // 非错误队列条目，忽略

                if (ee->ee_origin == SO_EE_ORIGIN_ICMP) {
                    const int type = ee->ee_type;
                    const int code = ee->ee_code;
                    hop_addr = ipv4_to_string(SO_EE_OFFENDER(ee));
                    if (type == ICMP_TIME_EXCEEDED) {
                        replied = true;
                        is_reached = false;
                        break;
                    }
                    if (type == ICMP_DEST_UNREACH) {
                        replied = true;
                        is_reached = (code == ICMP_UNREACH_PORT);
                        done = true;  // 目的不可达（端口/主机/网络等）均为终点
                        break;
                    }
                    // 其它 ICMP（重定向/源抑制等）忽略，继续等待
                }
                // SO_EE_ORIGIN_LOCAL 等本地错误不参与判定，继续等待
            }

            if (replied) {
                rtt_ms = (monotonic_seconds() - send_time) * 1000.0;
                if (hop.addr.empty()) hop.addr = hop_addr;  // 一跳只记录首个回复地址（经典 traceroute 行为）
                if (is_reached) {
                    hop.reached = true;
                    reached = true;
                }
            } else {
                ++hop.loss;
            }
            hop.rtt_ms.push_back(rtt_ms);
        }

        result.hops.push_back(hop);
    }

    close(fd);
    result.status = reached ? "reached" : "completed";
    return result;
}

}  // namespace

TracerouteResult trace_route(const std::string &host, uint16_t port,
                             const TracerouteConfig &config) {
    if (host.empty() || port == 0) {
        TracerouteResult result;
        result.host = host;
        result.port = port;
        result.status = "error";
        result.error = "invalid argument";
        return result;
    }
    return trace_route_impl(host, port, config);
}

std::string trace_route_json(const std::string &host, uint16_t port,
                             const TracerouteConfig &config) {
    const TracerouteResult r = trace_route(host, port, config);
    std::ostringstream out;
    out << "{\"host\":\"" << json_escape(r.host) << "\""
        << ",\"port\":" << r.port
        << ",\"resolved_ip\":\"" << json_escape(r.resolved_ip) << "\""
        << ",\"ip_version\":" << r.ip_version
        << ",\"max_hops\":" << r.max_hops
        << ",\"probes_per_hop\":" << r.probes_per_hop
        << ",\"status\":\"" << json_escape(r.status) << "\""
        << ",\"error\":\"" << json_escape(r.error) << "\""
        << ",\"hops\":[";
    for (size_t i = 0; i < r.hops.size(); ++i) {
        if (i != 0) out << ",";
        const TracerouteHop &h = r.hops[i];
        double min_rtt = -1;
        double max_rtt = -1;
        double sum = 0;
        int count = 0;
        for (const double v : h.rtt_ms) {
            if (v < 0) continue;
            if (min_rtt < 0 || v < min_rtt) min_rtt = v;
            if (v > max_rtt) max_rtt = v;
            sum += v;
            ++count;
        }
        const double avg_rtt = count > 0 ? sum / count : -1;

        out << "{\"ttl\":" << h.ttl
            << ",\"addr\":\"" << json_escape(h.addr) << "\""
            << ",\"rtt_ms\":[";
        for (size_t k = 0; k < h.rtt_ms.size(); ++k) {
            if (k != 0) out << ",";
            out << rtt_to_string(h.rtt_ms[k]);
        }
        out << "]"
            << ",\"min_rtt_ms\":" << rtt_to_string(min_rtt)
            << ",\"avg_rtt_ms\":" << rtt_to_string(avg_rtt)
            << ",\"max_rtt_ms\":" << rtt_to_string(max_rtt)
            << ",\"loss\":" << h.loss
            << ",\"reached\":" << (h.reached ? "true" : "false")
            << "}";
    }
    out << "]}";
    return out.str();
}

}  // namespace sdt
}  // namespace asternet

#else  // 非 Linux/Android 平台：无 IP_RECVERR，返回兜底错误

namespace asternet {
namespace sdt {

TracerouteResult trace_route(const std::string &host, uint16_t port,
                             const TracerouteConfig & /*config*/) {
    TracerouteResult result;
    result.host = host;
    result.port = port;
    result.status = "error";
    result.error = "traceroute unsupported on this platform";
    return result;
}

std::string trace_route_json(const std::string &host, uint16_t port,
                             const TracerouteConfig &config) {
    const TracerouteResult r = trace_route(host, port, config);
    std::ostringstream out;
    out << "{\"host\":\"" << json_escape(host) << "\""
        << ",\"port\":" << port
        << ",\"resolved_ip\":\"\""
        << ",\"ip_version\":0"
        << ",\"max_hops\":" << config.max_hops
        << ",\"probes_per_hop\":" << config.probes_per_hop
        << ",\"status\":\"error\""
        << ",\"error\":\"traceroute unsupported on this platform\""
        << ",\"hops\":[]}";
    return out.str();
}

}  // namespace sdt
}  // namespace asternet

#endif  // ASTERNET_PLATFORM_LINUX / ANDROID
