#include "dns/dns_resolver.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace asternet {
namespace dns {

namespace {

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool is_numeric_ip(const std::string &value, bool *ipv6) {
    struct in_addr v4 {};
    if (inet_pton(AF_INET, value.c_str(), &v4) == 1) {
        if (ipv6 != nullptr) *ipv6 = false;
        return true;
    }
    struct in6_addr v6 {};
    if (inet_pton(AF_INET6, value.c_str(), &v6) == 1) {
        if (ipv6 != nullptr) *ipv6 = true;
        return true;
    }
    return false;
}

bool is_safe_ipv4(const struct in_addr &v4, bool allow_private_addresses) {
    if (allow_private_addresses) return true;
    const uint32_t address = ntohl(v4.s_addr);
    const uint8_t a = static_cast<uint8_t>(address >> 24);
    const uint8_t b = static_cast<uint8_t>(address >> 16);
    return a != 0 && a != 10 && a != 127 && a < 224
        && !(a == 100 && b >= 64 && b <= 127)
        && !(a == 169 && b == 254)
        && !(a == 172 && b >= 16 && b <= 31)
        && !(a == 192 && b == 168)
        && !(a == 198 && (b == 18 || b == 19));
}

bool check_embedded_ipv4(const struct in6_addr &v6, size_t offset,
                         bool allow_private_addresses, bool *ipv6) {
    struct in_addr embedded4 {};
    std::memcpy(&embedded4, &v6.s6_addr[offset], sizeof(embedded4));
    if (ipv6 != nullptr) *ipv6 = true;
    return is_safe_ipv4(embedded4, allow_private_addresses);
}

bool is_safe_ip(const std::string &value, bool allow_private_addresses, bool *ipv6) {
    struct in_addr v4 {};
    if (inet_pton(AF_INET, value.c_str(), &v4) == 1) {
        if (ipv6 != nullptr) *ipv6 = false;
        return is_safe_ipv4(v4, allow_private_addresses);
    }

    struct in6_addr v6 {};
    if (inet_pton(AF_INET6, value.c_str(), &v6) != 1) return false;
    if (ipv6 != nullptr) *ipv6 = true;
    if (IN6_IS_ADDR_V4MAPPED(&v6)) {
        return check_embedded_ipv4(v6, 12, allow_private_addresses, ipv6);
    }
    bool ipv4_compatible = true;
    for (size_t i = 0; i < 12; ++i) ipv4_compatible = ipv4_compatible && v6.s6_addr[i] == 0x00;
    if (ipv4_compatible && !(v6.s6_addr[12] == 0x00 && v6.s6_addr[13] == 0x00
        && v6.s6_addr[14] == 0x00 && (v6.s6_addr[15] == 0x00 || v6.s6_addr[15] == 0x01))) {
        return check_embedded_ipv4(v6, 12, allow_private_addresses, ipv6);
    }
    if (v6.s6_addr[0] == 0x00 && v6.s6_addr[1] == 0x64 && v6.s6_addr[2] == 0xff
        && v6.s6_addr[3] == 0x9b && v6.s6_addr[4] == 0x00 && v6.s6_addr[5] == 0x00
        && v6.s6_addr[6] == 0x00 && v6.s6_addr[7] == 0x00 && v6.s6_addr[8] == 0x00
        && v6.s6_addr[9] == 0x00 && v6.s6_addr[10] == 0x00 && v6.s6_addr[11] == 0x00) {
        return check_embedded_ipv4(v6, 12, allow_private_addresses, ipv6);
    }
    if (!allow_private_addresses && v6.s6_addr[0] == 0x00 && v6.s6_addr[1] == 0x64
        && v6.s6_addr[2] == 0xff && v6.s6_addr[3] == 0x9b && v6.s6_addr[4] == 0x00
        && v6.s6_addr[5] == 0x01) {
        return false;
    }
    if (v6.s6_addr[0] == 0x00 && v6.s6_addr[1] == 0x00 && v6.s6_addr[2] == 0x00
        && v6.s6_addr[3] == 0x00 && v6.s6_addr[4] == 0x00 && v6.s6_addr[5] == 0x00
        && v6.s6_addr[6] == 0x00 && v6.s6_addr[7] == 0x00 && v6.s6_addr[8] == 0xff
        && v6.s6_addr[9] == 0xff && v6.s6_addr[10] == 0x00 && v6.s6_addr[11] == 0x00) {
        return check_embedded_ipv4(v6, 12, allow_private_addresses, ipv6);
    }
    if (v6.s6_addr[0] == 0x20 && v6.s6_addr[1] == 0x02) {
        return check_embedded_ipv4(v6, 2, allow_private_addresses, ipv6);
    }
    if (allow_private_addresses) return true;
    static constexpr uint8_t kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t *bytes = v6.s6_addr;
    bool unspecified = true;
    for (size_t i = 0; i < sizeof(v6.s6_addr); ++i) unspecified = unspecified && bytes[i] == 0;
    if (unspecified || std::memcmp(bytes, kLoopback, sizeof(kLoopback)) == 0) return false;
    if ((bytes[0] & 0xfe) == 0xfc || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)
        || bytes[0] == 0xff) {
        return false;
    }
    return true;
}

struct LookupJob {
    std::mutex mutex;
    std::condition_variable complete;
    bool done = false;
    std::vector<IpResult> addresses;
};

std::vector<IpResult> resolve_local(const std::string &host, bool allow_private_addresses) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *raw = nullptr;
    std::vector<IpResult> addresses;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &raw) != 0 || raw == nullptr) {
        if (raw != nullptr) freeaddrinfo(raw);
        return addresses;
    }

    std::unordered_set<std::string> seen;
    for (struct addrinfo *item = raw; item != nullptr; item = item->ai_next) {
        char text[INET6_ADDRSTRLEN] = {};
        const void *address = nullptr;
        if (item->ai_family == AF_INET) {
            address = &reinterpret_cast<struct sockaddr_in *>(item->ai_addr)->sin_addr;
        } else if (item->ai_family == AF_INET6) {
            address = &reinterpret_cast<struct sockaddr_in6 *>(item->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (inet_ntop(item->ai_family, address, text, sizeof(text)) == nullptr) continue;
        bool ipv6 = false;
        if (!is_safe_ip(text, allow_private_addresses, &ipv6) || !seen.insert(text).second) continue;
        IpResult resolved;
        resolved.ip = text;
        resolved.ipv6 = ipv6;
        resolved.source = ResolutionSource::kLocalDns;
        addresses.push_back(std::move(resolved));
    }
    freeaddrinfo(raw);
    return addresses;
}

std::string cache_key(const std::string &host, uint64_t network_epoch) {
    return host + '\n' + std::to_string(network_epoch);
}

std::string health_key(const std::string &host, const std::string &ip, uint64_t network_epoch) {
    return cache_key(host, network_epoch) + '\n' + ip;
}

}  // namespace

struct SmartDnsResolverImpl::State {
    struct CacheEntry {
        std::vector<IpResult> addresses;
        int64_t expires_at_ms = 0;
        int64_t stale_until_ms = 0;
    };

    struct Health {
        int rtt_ms = -1;
        int failures = 0;
        int successes = 0;
        int64_t updated_at_ms = 0;
    };

    explicit State(Config cfg, HttpDnsLookup lookup)
        : config(std::move(cfg)), httpdns_lookup(std::move(lookup)) {}

    Config config;
    mutable std::mutex mutex;
    HttpDnsLookup httpdns_lookup;
    std::unordered_map<std::string, CacheEntry> cache;
    std::unordered_map<std::string, Health> health;
    std::unordered_map<std::string, std::vector<IpResult>> backup_ips;
    sdt::QualitySnapshot quality;
    uint64_t last_network_epoch = 0;
    size_t active_lookups = 0;
};

SmartDnsResolverImpl::SmartDnsResolverImpl() : SmartDnsResolverImpl(Config{}, {}) {}

SmartDnsResolverImpl::SmartDnsResolverImpl(Config config, HttpDnsLookup httpdns_lookup)
    : state_(std::make_shared<State>(std::move(config), std::move(httpdns_lookup))) {}

SmartDnsResolverImpl::~SmartDnsResolverImpl() = default;

std::vector<IpResult> SmartDnsResolverImpl::resolve(const std::string &host) {
    return resolve_with_metadata(host, 0).addresses;
}

ResolveResult SmartDnsResolverImpl::resolve_with_metadata(const std::string &host,
                                                           uint64_t network_epoch, int timeout_ms) {
    ResolveResult result;
    const int64_t started_ms = monotonic_ms();
    if (host.empty()) {
        result.error = ASTERNET_ERR_INVALID_ARGUMENT;
        return result;
    }

    bool literal_ipv6 = false;
    if (is_numeric_ip(host, &literal_ipv6)) {
        if (!is_safe_ip(host, state_->config.allow_private_addresses, &literal_ipv6)) {
            result.error = ASTERNET_ERR_DNS;
            result.elapsed_ms = monotonic_ms() - started_ms;
            return result;
        }
        IpResult literal;
        literal.ip = host;
        literal.ipv6 = literal_ipv6;
        literal.score = 10000;
        literal.source = ResolutionSource::kLiteral;
        result.addresses.push_back(std::move(literal));
        result.elapsed_ms = monotonic_ms() - started_ms;
        return result;
    }

    const std::string key = cache_key(host, network_epoch);
    State::CacheEntry stale_entry;
    bool has_stale_entry = false;
    HttpDnsLookup httpdns_lookup;
    std::vector<IpResult> addresses;
    std::vector<IpResult> backup_ips;
    Config config;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto cached = state_->cache.find(key);
        if (cached != state_->cache.end()) {
            const int64_t now_ms = monotonic_ms();
            if (now_ms <= cached->second.expires_at_ms) {
                addresses = cached->second.addresses;
                result.cache_hit = true;
            }
            stale_entry = cached->second;
            has_stale_entry = now_ms <= cached->second.stale_until_ms;
        }
        httpdns_lookup = state_->httpdns_lookup;
        config = state_->config;
        const auto backup = state_->backup_ips.find(host);
        if (backup != state_->backup_ips.end()) backup_ips = backup->second;
    }

    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : config.default_lookup_timeout_ms;
    const int64_t deadline_ms = started_ms + std::max(1, effective_timeout_ms);
    bool lookup_timed_out = false;
    auto run_bounded_lookup = [this, &config, deadline_ms, &lookup_timed_out](
                                  std::function<std::vector<IpResult>()> lookup,
                                  std::vector<IpResult> &addresses) -> bool {
        auto job = std::make_shared<LookupJob>();
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->active_lookups >= config.max_active_lookups) {
                lookup_timed_out = true;
                return false;
            }
            ++state_->active_lookups;
        }
        const std::shared_ptr<State> state = state_;
        try {
            std::thread([state, job, lookup = std::move(lookup)]() mutable {
                std::vector<IpResult> resolved;
                try {
                    resolved = lookup();
                } catch (...) {
                    resolved.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->addresses = std::move(resolved);
                    job->done = true;
                }
                job->complete.notify_all();
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->active_lookups > 0) --state->active_lookups;
            }).detach();
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->active_lookups > 0) --state_->active_lookups;
            return false;
        }

        std::unique_lock<std::mutex> lock(job->mutex);
        const auto deadline = std::chrono::steady_clock::time_point(std::chrono::milliseconds(deadline_ms));
        if (!job->complete.wait_until(lock, deadline, [&] { return job->done; })) {
            lookup_timed_out = true;
            return false;
        }
        addresses = std::move(job->addresses);
        return true;
    };

    sdt::QualitySnapshot quality_snapshot;
    bool allow_live_lookup = true;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        quality_snapshot = state_->quality;
        allow_live_lookup = quality_snapshot.quality != sdt::NetworkQuality::kOffline;
    }
    if (addresses.empty() && httpdns_lookup && monotonic_ms() < deadline_ms && allow_live_lookup) {
        run_bounded_lookup([httpdns_lookup, host] { return httpdns_lookup(host); }, addresses);
        addresses.erase(std::remove_if(addresses.begin(), addresses.end(), [&config](IpResult &item) {
            bool ipv6 = false;
            if (!is_safe_ip(item.ip, config.allow_private_addresses, &ipv6)) return true;
            item.ipv6 = ipv6;
            item.source = ResolutionSource::kHttpDns;
            return false;
        }), addresses.end());
    }

    if (addresses.empty() && monotonic_ms() < deadline_ms && allow_live_lookup) {
        run_bounded_lookup([host, allow_private = config.allow_private_addresses] {
            return resolve_local(host, allow_private);
        }, addresses);
    }

    if (addresses.empty()) addresses = std::move(backup_ips);
    if (addresses.empty() && has_stale_entry) {
        addresses = stale_entry.addresses;
        result.cache_hit = true;
    }
    addresses.erase(std::remove_if(addresses.begin(), addresses.end(), [&config](IpResult &item) {
        bool ipv6 = false;
        if (!is_safe_ip(item.ip, config.allow_private_addresses, &ipv6)) return true;
        item.ipv6 = ipv6;
        if (item.source != ResolutionSource::kHttpDns && item.source != ResolutionSource::kLocalDns) {
            item.source = ResolutionSource::kBackup;
        }
        return false;
    }), addresses.end());

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        quality_snapshot = state_->quality;
        const int64_t now_ms = monotonic_ms();
        for (IpResult &item : addresses) {
            const auto health = state_->health.find(health_key(host, item.ip, network_epoch));
            const State::Health *entry = health == state_->health.end() ? nullptr : &health->second;
            const int rtt = entry != nullptr && entry->rtt_ms >= 0 ? entry->rtt_ms : item.rtt_ms;
            const int failures = entry != nullptr ? entry->failures : 0;
            const int successes = entry != nullptr ? entry->successes : 0;
            const int loss = item.loss_rate < 0 ? 0 : item.loss_rate;
            int quality_penalty = 0;
            switch (quality_snapshot.quality) {
            case sdt::NetworkQuality::kGood: quality_penalty = 0; break;
            case sdt::NetworkQuality::kDegraded: quality_penalty = rtt < 0 ? 300 : 100; break;
            case sdt::NetworkQuality::kBad: quality_penalty = rtt < 0 ? 700 : 250; break;
            case sdt::NetworkQuality::kOffline: quality_penalty = rtt < 0 ? 1000 : 500; break;
            case sdt::NetworkQuality::kUnknown: quality_penalty = 50; break;
            }
            const int success_bonus = quality_snapshot.quality == sdt::NetworkQuality::kGood
                ? 0 : std::min(500, successes * 100);
            // 高分优先：失败会比单次 RTT 更强地降低排序，避免反复命中坏地址。
            item.rtt_ms = rtt;
            const int rtt_penalty = rtt < 0 ? 500 : std::min(8000, rtt * 2);
            item.score = 10000 - rtt_penalty
                       - std::min(2000, loss * 2) - std::min(4000, failures * 500)
                       - quality_penalty + success_bonus;
        }
        std::stable_sort(addresses.begin(), addresses.end(), [](const IpResult &left,
                                                                  const IpResult &right) {
            if (left.score != right.score) return left.score > right.score;
            // RFC 8305 仍由连接层做错峰竞速；此处仅让排序稳定。
            return left.ip < right.ip;
        });
        if (!addresses.empty()) {
            if (state_->cache.size() >= state_->config.max_cache_entries
                && state_->cache.find(key) == state_->cache.end()) {
                state_->cache.erase(state_->cache.begin());
            }
            state_->cache[key] = {addresses, now_ms + state_->config.ttl_ms,
                                  now_ms + state_->config.ttl_ms + state_->config.stale_ttl_ms};
        }
    }

    result.addresses = std::move(addresses);
    result.error = result.addresses.empty()
        ? (lookup_timed_out ? ASTERNET_ERR_TIMEOUT : ASTERNET_ERR_DNS) : ASTERNET_OK;
    result.elapsed_ms = monotonic_ms() - started_ms;
    return result;
}

int SmartDnsResolverImpl::prefetch(const std::string &host) {
    return resolve_with_metadata(host, 0).error;
}

int SmartDnsResolverImpl::prefetch(const std::string &host, uint64_t network_epoch) {
    return resolve_with_metadata(host, network_epoch).error;
}

void SmartDnsResolverImpl::invalidate(const std::string &host) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (host.empty()) {
        state_->cache.clear();
        return;
    }
    const std::string prefix = host + '\n';
    for (auto it = state_->cache.begin(); it != state_->cache.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = state_->cache.erase(it);
        } else {
            ++it;
        }
    }
}

void SmartDnsResolverImpl::report_connection_result(const std::string &host, const std::string &ip,
                                                      uint64_t network_epoch, bool success,
                                                      int rtt_ms) {
    if (host.empty() || ip.empty()) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::string key = health_key(host, ip, network_epoch);
    if (state_->health.size() >= state_->config.max_health_entries
        && state_->health.find(key) == state_->health.end()) {
        auto oldest = state_->health.end();
        for (auto it = state_->health.begin(); it != state_->health.end(); ++it) {
            if (oldest == state_->health.end()
                || it->second.updated_at_ms < oldest->second.updated_at_ms) {
                oldest = it;
            }
        }
        if (oldest != state_->health.end()) state_->health.erase(oldest);
    }
    State::Health &health = state_->health[key];
    health.updated_at_ms = monotonic_ms();
    if (success) {
        ++health.successes;
        health.failures = 0;
        if (rtt_ms >= 0) {
            health.rtt_ms = health.rtt_ms < 0 ? rtt_ms : (health.rtt_ms * 7 + rtt_ms * 3) / 10;
        }
    } else {
        ++health.failures;
    }
}

void SmartDnsResolverImpl::on_quality_change(const sdt::QualitySnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->quality = snapshot;
}

void SmartDnsResolverImpl::on_network_change(uint64_t network_epoch) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->last_network_epoch = network_epoch;
    // 旧网络的地址质量不可直接迁移到新网络。
    state_->health.clear();
}

std::string SmartDnsResolverImpl::dump() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::ostringstream out;
    out << "{\"entries\":" << state_->cache.size() << ",\"health\":"
        << state_->health.size() << ",\"network_epoch\":" << state_->last_network_epoch << "}";
    return out.str();
}

void SmartDnsResolverImpl::set_httpdns_lookup(HttpDnsLookup httpdns_lookup) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->httpdns_lookup = std::move(httpdns_lookup);
    state_->cache.clear();
}

void SmartDnsResolverImpl::set_backup_ips(const std::string &host,
                                          std::vector<IpResult> addresses) {
    std::vector<IpResult> safe_addresses;
    safe_addresses.reserve(addresses.size());
    for (IpResult &item : addresses) {
        bool ipv6 = false;
        if (!is_numeric_ip(item.ip, &ipv6)) continue;
        if (!is_safe_ip(item.ip, state_->config.allow_private_addresses, &ipv6)) continue;
        item.ipv6 = ipv6;
        item.source = ResolutionSource::kBackup;
        safe_addresses.push_back(std::move(item));
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (safe_addresses.empty()) {
        state_->backup_ips.erase(host);
    } else {
        state_->backup_ips[host] = std::move(safe_addresses);
    }
    const std::string prefix = host + '\n';
    for (auto it = state_->cache.begin(); it != state_->cache.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = state_->cache.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace dns
}  // namespace asternet
