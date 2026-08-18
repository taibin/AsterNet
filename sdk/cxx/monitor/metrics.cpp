#include "monitor/metrics.h"

#include <deque>
#include <mutex>
#include <sstream>
#include <utility>

namespace asternet {
namespace monitor {

struct MetricsCollectorImpl::State {
    explicit State(size_t max) : max_events(max == 0 ? 1 : max) {}

    mutable std::mutex mutex;
    size_t max_events;
    MetricsSnapshot snapshot;
    std::deque<RequestMetrics> events;
};

namespace {

void update_stage(StageStats &stage, bool started, bool succeeded, bool failed, int64_t ms) {
    if (started) ++stage.started;
    if (succeeded) ++stage.succeeded;
    if (failed) ++stage.failed;
    if (ms >= 0) {
        ++stage.samples;
        stage.total_ms += ms;
        if (stage.min_ms < 0 || ms < stage.min_ms) stage.min_ms = ms;
        if (ms > stage.max_ms) stage.max_ms = ms;
    }
}

std::string escape_json(const std::string &input) {
    std::string out;
    out.reserve(input.size() + 8);
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += kHex[(c >> 4) & 0x0f];
                out += kHex[c & 0x0f];
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string stage_json(const StageStats &stage) {
    std::ostringstream out;
    const double success_rate = stage.success_rate();
    out << "{\"started\":" << stage.started
        << ",\"succeeded\":" << stage.succeeded
        << ",\"failed\":" << stage.failed
        << ",\"success_rate\":";
    if (stage.started == 0) out << -1;
    else out << success_rate;
    out << ",\"avg_ms\":" << stage.average_ms()
        << ",\"min_ms\":" << stage.min_ms
        << ",\"max_ms\":" << stage.max_ms << "}";
    return out.str();
}

std::string snapshot_json(const MetricsSnapshot &snapshot) {
    std::ostringstream out;
    out << "{\"events\":" << snapshot.events
        << ",\"metrics\":";
    out << "{\"requests\":" << snapshot.requests
        << ",\"success\":" << snapshot.success
        << ",\"failure\":" << snapshot.failure
        << ",\"degraded\":" << snapshot.degraded
        << ",\"deduplicated\":" << snapshot.deduplicated
        << ",\"connection_reused\":" << snapshot.connection_reused
        << ",\"cache_hit\":" << snapshot.cache_hit
        << ",\"attempts\":" << snapshot.attempts
        << ",\"avg_total_ms\":" << snapshot.avg_total_ms
        << ",\"max_total_ms\":" << snapshot.max_total_ms
        << ",\"stages\":{"
        << "\"dns\":" << stage_json(snapshot.dns) << ','
        << "\"connect\":" << stage_json(snapshot.connect) << ','
        << "\"tls\":" << stage_json(snapshot.tls) << ','
        << "\"ttfb\":" << stage_json(snapshot.ttfb) << ','
        << "\"transfer\":" << stage_json(snapshot.transfer) << ','
        << "\"total\":" << stage_json(snapshot.total) << "}";
    out << ",\"failure_stages\":{";
    bool first = true;
    for (const auto &entry : snapshot.failure_stages) {
        if (!first) out << ',';
        first = false;
        out << '"' << escape_json(entry.first) << "\":" << entry.second;
    }
    out << "}}";
    out << "}";
    return out.str();
}

void tally_failure_stage(std::map<std::string, size_t> &stages, const std::string &stage) {
    if (stage.empty()) return;
    std::string normalized = stage.size() > 64 ? stage.substr(0, 64) : stage;
    auto it = stages.find(normalized);
    if (it != stages.end()) {
        ++it->second;
        return;
    }
    static constexpr size_t kMaxFailureStages = 32;
    if (stages.size() >= kMaxFailureStages) {
        ++stages["other"];
        return;
    }
    stages.emplace(std::move(normalized), 1);
}

}  // namespace

double StageStats::success_rate() const {
    if (started == 0) return -1.0;
    return static_cast<double>(succeeded) / static_cast<double>(started);
}

int64_t StageStats::average_ms() const {
    if (samples == 0) return -1;
    return total_ms / static_cast<int64_t>(samples);
}

MetricsCollectorImpl::MetricsCollectorImpl(size_t max_events)
    : state_(std::make_unique<State>(max_events)) {}

MetricsCollectorImpl::~MetricsCollectorImpl() = default;

void MetricsCollectorImpl::report(const asternet_response_info_t &metrics) {
    RequestMetrics event;
    event.response = metrics;
    report_request(event);
}

void MetricsCollectorImpl::report_request(const RequestMetrics &metrics) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        MetricsSnapshot &snapshot = state_->snapshot;
        ++snapshot.requests;
        snapshot.attempts += metrics.attempts > 0 ? static_cast<size_t>(metrics.attempts) : 1;
        if (metrics.response.result == ASTERNET_OK) ++snapshot.success;
        else ++snapshot.failure;
        if (metrics.response.degraded != 0) ++snapshot.degraded;
        if (metrics.connection_reused) ++snapshot.connection_reused;
        if (metrics.cache_hit) ++snapshot.cache_hit;
        if (metrics.deduplicated) ++snapshot.deduplicated;
        if (metrics.response.total_ms >= 0) {
            if (snapshot.max_total_ms < 0 || metrics.response.total_ms > snapshot.max_total_ms) {
                snapshot.max_total_ms = metrics.response.total_ms;
            }
        }
        const bool dns_failed = metrics.response.result == ASTERNET_ERR_DNS
            || metrics.failure_stage == "dns";
        update_stage(snapshot.dns, true, !dns_failed, dns_failed, metrics.response.dns_ms);
        const bool connect_started = !dns_failed && !metrics.connection_reused
            && (metrics.response.connect_ms >= 0
                || metrics.response.result == ASTERNET_ERR_CONNECT
                || metrics.failure_stage == "connect");
        const bool connect_failed = metrics.response.result == ASTERNET_ERR_CONNECT
            || metrics.failure_stage == "connect";
        if (connect_started) {
            update_stage(snapshot.connect, true, !connect_failed, connect_failed,
                         metrics.response.connect_ms);
        }
        const bool h3_handshake_timeout = metrics.response.protocol == ASTERNET_PROTOCOL_HTTP_3
            && metrics.failure_stage == "timeout"
            && metrics.response.tls_ms < 0
            && metrics.response.ttfb_ms < 0;
        const bool tls_started = connect_started && !connect_failed
            && (metrics.response.tls_ms >= 0
                || metrics.response.result == ASTERNET_ERR_TLS
                || metrics.failure_stage == "tls"
                || h3_handshake_timeout);
        const bool tls_failed = metrics.response.result == ASTERNET_ERR_TLS
            || metrics.failure_stage == "tls"
            || h3_handshake_timeout;
        if (tls_started) {
            update_stage(snapshot.tls, true, !tls_failed, tls_failed, metrics.response.tls_ms);
        }
        const bool ttfb_started = tls_started && !tls_failed
            && (metrics.response.ttfb_ms >= 0 || metrics.failure_stage == "ttfb"
                || metrics.failure_stage == "timeout");
        if (ttfb_started) {
            const bool ttfb_failed = metrics.response.ttfb_ms < 0;
            update_stage(snapshot.ttfb, true, !ttfb_failed, ttfb_failed, metrics.response.ttfb_ms);
        }
        const bool transfer_started = metrics.response.ttfb_ms >= 0;
        if (transfer_started) {
            const int64_t transfer_ms = metrics.response.total_ms >= 0 && metrics.response.ttfb_ms >= 0
                ? metrics.response.total_ms - metrics.response.ttfb_ms
                : -1;
            update_stage(snapshot.transfer, true, metrics.response.result == ASTERNET_OK,
                         metrics.response.result != ASTERNET_OK, transfer_ms);
        }
        update_stage(snapshot.total, true, metrics.response.result == ASTERNET_OK,
                     metrics.response.result != ASTERNET_OK, metrics.response.total_ms);
        tally_failure_stage(snapshot.failure_stages,
            !metrics.failure_stage.empty() ? metrics.failure_stage
                                           : (metrics.response.result == ASTERNET_OK ? ""
                                              : "internal"));
        if (state_->events.size() == state_->max_events) state_->events.pop_front();
        state_->events.push_back(metrics);
        snapshot.events = state_->events.size();
    }
}

MetricsSnapshot MetricsCollectorImpl::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    MetricsSnapshot snapshot = state_->snapshot;
    snapshot.avg_total_ms = snapshot.total.average_ms();
    return snapshot;
}

std::string MetricsCollectorImpl::dump() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    MetricsSnapshot snapshot = state_->snapshot;
    snapshot.avg_total_ms = snapshot.total.average_ms();
    std::ostringstream out;
    out << snapshot_json(snapshot);
    return out.str();
}

std::vector<RequestMetrics> MetricsCollectorImpl::recent_events() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->events.begin(), state_->events.end()};
}

}  // namespace monitor
}  // namespace asternet
