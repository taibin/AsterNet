#include "monitor/metrics.h"

#include <deque>
#include <mutex>
#include <sstream>
#include <utility>

namespace asternet {
namespace monitor {

struct MetricsCollectorImpl::State {
    explicit State(size_t max, Reporter callback) : max_events(max == 0 ? 1 : max), reporter(std::move(callback)) {}

    mutable std::mutex mutex;
    size_t max_events;
    size_t success_count = 0;
    size_t failure_count = 0;
    Reporter reporter;
    std::deque<RequestMetrics> events;
};

MetricsCollectorImpl::MetricsCollectorImpl(size_t max_events, Reporter reporter)
    : state_(std::make_unique<State>(max_events, std::move(reporter))) {}

MetricsCollectorImpl::~MetricsCollectorImpl() = default;

void MetricsCollectorImpl::report(const asternet_response_info_t &metrics) {
    RequestMetrics event;
    event.response = metrics;
    report_request(event);
}

void MetricsCollectorImpl::report_request(const RequestMetrics &metrics) {
    Reporter reporter;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (metrics.response.result == ASTERNET_OK) ++state_->success_count;
        else ++state_->failure_count;
        if (state_->events.size() == state_->max_events) state_->events.pop_front();
        state_->events.push_back(metrics);
        reporter = state_->reporter;
    }
    // Callback is invoked after internal state is released. Hosts must dispatch it off network threads.
    if (reporter) reporter(metrics);
}

std::string MetricsCollectorImpl::dump() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::ostringstream out;
    out << "{\"events\":" << state_->events.size() << ",\"success\":"
        << state_->success_count << ",\"failure\":" << state_->failure_count << "}";
    return out.str();
}

std::vector<RequestMetrics> MetricsCollectorImpl::recent_events() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->events.begin(), state_->events.end()};
}

void MetricsCollectorImpl::set_reporter(Reporter reporter) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->reporter = std::move(reporter);
}

}  // namespace monitor
}  // namespace asternet
