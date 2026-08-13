#include "connection/connection_pool.h"

#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace asternet {
namespace connection {

namespace {

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string key_for(const Origin &origin) {
    return origin.host + '\n' + std::to_string(origin.port) + '\n'
        + std::to_string(static_cast<int>(origin.protocol)) + '\n'
        + std::to_string(origin.network_epoch);
}

}  // namespace

struct ConnectionPoolImpl::State {
    struct Entry {
        size_t active_leases = 0;
        size_t successful_requests = 0;
        size_t failed_requests = 0;
        int64_t last_used_ms = 0;
    };

    explicit State(size_t max) : max_origins(max) {}

    mutable std::mutex mutex;
    size_t max_origins;
    uint64_t next_lease_id = 1;
    uint64_t network_epoch = 0;
    size_t prefetches = 0;
    size_t migrations = 0;
    size_t evictions = 0;
    int last_prefetch_result = ASTERNET_ERR_UNSUPPORTED;
    int last_migration_result = ASTERNET_ERR_UNSUPPORTED;
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<uint64_t, std::string> leases;
    PrefetchHandler prefetch_handler;
    MigrationHandler migration_handler;
};

ConnectionPoolImpl::ConnectionPoolImpl(size_t max_origins)
    : state_(std::make_unique<State>(max_origins == 0 ? 1 : max_origins)) {}

ConnectionPoolImpl::~ConnectionPoolImpl() = default;

int ConnectionPoolImpl::prefetch(const std::string &host) {
    if (host.empty()) return ASTERNET_ERR_INVALID_ARGUMENT;
    PrefetchHandler handler;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->prefetches;
        handler = state_->prefetch_handler;
    }
    // Without a persistent transport implementation, report UNSUPPORTED rather than a fake success.
    const int result = handler ? handler(host) : ASTERNET_ERR_UNSUPPORTED;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->last_prefetch_result = result;
    }
    return result;
}

int ConnectionPoolImpl::migrate(asternet_network_t /*new_net*/) {
    MigrationHandler handler;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->migrations;
        handler = state_->migration_handler;
    }
    const int result = handler ? handler() : ASTERNET_ERR_UNSUPPORTED;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->last_migration_result = result;
    }
    return result;
}

void ConnectionPoolImpl::evict_all() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->evictions += state_->entries.size();
    state_->entries.clear();
    state_->leases.clear();
}

ConnectionLease ConnectionPoolImpl::acquire(const Origin &origin) {
    if (origin.host.empty()) return {};
    const std::string key = key_for(origin);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->entries.size() >= state_->max_origins && state_->entries.find(key) == state_->entries.end()) {
        // Entries contain only accounting metadata today. Evict the oldest idle entry first.
        auto oldest = state_->entries.end();
        for (auto it = state_->entries.begin(); it != state_->entries.end(); ++it) {
            if (it->second.active_leases != 0) continue;
            if (oldest == state_->entries.end() || it->second.last_used_ms < oldest->second.last_used_ms) {
                oldest = it;
            }
        }
        if (oldest != state_->entries.end()) {
            state_->entries.erase(oldest);
            ++state_->evictions;
        }
        if (state_->entries.size() >= state_->max_origins) return {};
    }
    State::Entry &entry = state_->entries[key];
    ++entry.active_leases;
    entry.last_used_ms = monotonic_ms();
    ConnectionLease lease;
    lease.origin = origin;
    lease.id = state_->next_lease_id++;
    lease.valid = true;
    // No current engine exposes a reusable physical connection lease.
    lease.reused = false;
    state_->leases.emplace(lease.id, key);
    return lease;
}

void ConnectionPoolImpl::release(const ConnectionLease &lease, bool success) {
    if (!lease.valid || lease.id == 0) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto lease_it = state_->leases.find(lease.id);
    if (lease_it == state_->leases.end()) return;
    const auto entry_it = state_->entries.find(lease_it->second);
    if (entry_it != state_->entries.end()) {
        if (entry_it->second.active_leases > 0) --entry_it->second.active_leases;
        entry_it->second.last_used_ms = monotonic_ms();
        if (success) ++entry_it->second.successful_requests;
        else ++entry_it->second.failed_requests;
    }
    state_->leases.erase(lease_it);
}

void ConnectionPoolImpl::on_network_change(uint64_t network_epoch, asternet_network_t /*new_net*/) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->network_epoch = network_epoch;
    // Existing entries are bound to the preceding network epoch and cannot be reused safely.
    state_->evictions += state_->entries.size();
    state_->entries.clear();
    state_->leases.clear();
}

PoolSnapshot ConnectionPoolImpl::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    PoolSnapshot result;
    result.origins = state_->entries.size();
    result.active_leases = state_->leases.size();
    result.prefetches = state_->prefetches;
    result.migrations = state_->migrations;
    result.evictions = state_->evictions;
    result.last_prefetch_result = state_->last_prefetch_result;
    result.last_migration_result = state_->last_migration_result;
    result.network_epoch = state_->network_epoch;
    return result;
}

std::string ConnectionPoolImpl::dump() const {
    const PoolSnapshot current = snapshot();
    std::ostringstream out;
    out << "{\"origins\":" << current.origins << ",\"active_leases\":"
        << current.active_leases << ",\"prefetches\":" << current.prefetches
        << ",\"migrations\":" << current.migrations << ",\"evictions\":"
        << current.evictions << ",\"last_prefetch_result\":" << current.last_prefetch_result
        << ",\"last_migration_result\":" << current.last_migration_result
        << ",\"network_epoch\":" << current.network_epoch << "}";
    return out.str();
}

void ConnectionPoolImpl::set_prefetch_handler(PrefetchHandler handler) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->prefetch_handler = std::move(handler);
}

void ConnectionPoolImpl::set_migration_handler(MigrationHandler handler) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->migration_handler = std::move(handler);
}

}  // namespace connection
}  // namespace asternet
