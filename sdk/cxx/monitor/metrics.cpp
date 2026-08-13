#include "monitor/metrics.h"

#include <deque>
#include <mutex>
#include <sstream>

namespace asternet {
namespace monitor {

struct MetricsCollectorImpl::State {
    explicit State(size_t max) : max_events(max == 0 ? 1 : max) {}

    mutable std::mutex mutex;
    size_t max_events;
    size_t success_count = 0;
    size_t failure_count = 0;
    size_t degraded_count = 0;
    size_t deduplicated_count = 0;
    int64_t total_latency_ms = 0;
    int64_t max_latency_ms = 0;
    std::deque<RequestMetrics> events;
};

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
        if (metrics.response.result == ASTERNET_OK) ++state_->success_count;
        else ++state_->failure_count;
        if (metrics.response.degraded != 0) ++state_->degraded_count;
        if (metrics.deduplicated) ++state_->deduplicated_count;
        if (metrics.response.total_ms >= 0) {
            state_->total_latency_ms += metrics.response.total_ms;
            if (metrics.response.total_ms > state_->max_latency_ms) {
                state_->max_latency_ms = metrics.response.total_ms;
            }
        }
        if (state_->events.size() == state_->max_events) state_->events.pop_front();
        state_->events.push_back(metrics);
    }
}

std::string MetricsCollectorImpl::dump() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::ostringstream out;
    const size_t total = state_->success_count + state_->failure_count;
    const int64_t avg = total == 0 ? -1 : state_->total_latency_ms / static_cast<int64_t>(total);
    out << "{\"events\":" << state_->events.size() << ",\"success\":"
        << state_->success_count << ",\"failure\":" << state_->failure_count
        << ",\"degraded\":" << state_->degraded_count << ",\"deduplicated\":"
        << state_->deduplicated_count << ",\"avg_total_ms\":" << avg
        << ",\"max_total_ms\":" << state_->max_latency_ms << "}";
    return out.str();
}

std::vector<RequestMetrics> MetricsCollectorImpl::recent_events() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->events.begin(), state_->events.end()};
}

}  // namespace monitor
}  // namespace asternet
