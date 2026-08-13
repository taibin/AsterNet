#include "orchestrator/interceptor.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <functional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace asternet {
namespace orchestrator {

namespace {

constexpr size_t kMaxDedupPathBytes = 4096;
constexpr size_t kMaxDedupHeaderValueBytes = 1024;

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string lower_header_name(const std::string &name) {
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

bool has_visible_char(const std::string &value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

bool is_dedup_header(const std::string &name) {
    return name == "accept" || name == "accept-encoding" || name == "accept-language"
        || name == "content-type" || name == "range";
}

bool is_shareable(const RequestContext &context) {
    if (!context.request.retry_safe) return false;
    if (context.request.method != "GET" && context.request.method != "HEAD") return false;
    if (!context.request.body.empty()) return false;
    if (!context.request.connect_host.empty() || !context.request.ca_cert_pem.empty()) return false;
    if (context.request.path.size() > kMaxDedupPathBytes) return false;
    for (const engine::Header &header : context.request.headers) {
        const std::string name = lower_header_name(header.name);
        if (name == "authorization" || name == "cookie" || name == "proxy-authorization") return false;
        if (!is_dedup_header(name)) return false;
        if (is_dedup_header(name) && header.value.size() > kMaxDedupHeaderValueBytes) return false;
    }
    return true;
}

bool is_safe_method(const std::string &method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

bool has_idempotency_key(const std::vector<engine::Header> &headers) {
    for (const engine::Header &header : headers) {
        const std::string name = lower_header_name(header.name);
        if (name != "idempotency-key") continue;
        if (has_visible_char(header.value)) return true;
    }
    return false;
}

void normalize_retry_safety(RequestContext &context) {
    const bool replayable_safe_method = context.request.body.empty()
        && is_safe_method(context.request.method);
    const bool retry_safe = context.request.retry_safe || replayable_safe_method
        || (context.request.idempotent && has_idempotency_key(context.request.headers));
    context.request.retry_safe = retry_safe;
    context.max_retries = retry_safe ? std::max(context.max_retries, 1) : 0;
}

std::string dedup_key(const RequestContext &context) {
    std::ostringstream out;
    out << context.request.host << '\n' << context.request.port << '\n' << context.request.method
        << '\n' << context.request.path << '\n' << static_cast<int>(context.policy) << '\n'
        << context.network_epoch << '\n'
        << context.request.max_response_body_bytes << '\n' << context.request.timeout_ms << '\n'
        << context.max_retries << '\n';
    for (const engine::Header &header : context.request.headers) {
        const std::string name = lower_header_name(header.name);
        if (!is_dedup_header(name)) continue;
        out << name << ':' << header.value << '\n';
    }
    return out.str();
}

}  // namespace

int Chain::proceed(RequestContext &context, engine::Response &response) {
    if (index_ >= interceptors_.size()) return terminal_(context, response);
    Chain next(interceptors_, index_ + 1, terminal_);
    return interceptors_[index_]->intercept(next, context, response);
}

void InterceptorChain::add(std::unique_ptr<Interceptor> interceptor) {
    if (interceptor) interceptors_.push_back(std::move(interceptor));
}

int InterceptorChain::execute(RequestContext &context, engine::Response &response,
                              Terminal terminal) const {
    Chain initial(interceptors_, 0, std::move(terminal));
    return initial.proceed(context, response);
}

bool RetryInterceptor::is_retryable(int result) {
    return result == ASTERNET_ERR_DNS || result == ASTERNET_ERR_CONNECT
        || result == ASTERNET_ERR_TIMEOUT || result == ASTERNET_ERR_PROTOCOL
        || result == ASTERNET_ERR_UNSUPPORTED || result == ASTERNET_ERR_DEGRADED;
}

int RetryInterceptor::intercept(Chain &chain, RequestContext &context, engine::Response &response) {
    const int initial_timeout = context.request.timeout_ms;
    const int64_t fallback_deadline_ms = monotonic_ms() + initial_timeout;
    const int64_t deadline_ms = context.deadline_ms > 0 ? context.deadline_ms : fallback_deadline_ms;
    const int retry_limit = context.request.retry_safe ? std::max(0, context.max_retries) : 0;
    int result = ASTERNET_ERR_INTERNAL;
    for (int attempt = 0; attempt <= retry_limit; ++attempt) {
        const int remaining_ms = static_cast<int>(deadline_ms - monotonic_ms());
        if (remaining_ms <= 0) {
            response.err_code = ASTERNET_ERR_TIMEOUT;
            response.failure_stage = "deadline";
            return response.err_code;
        }
        context.request.timeout_ms = remaining_ms;
        ++context.attempts;
        response = {};
        result = chain.proceed(context, response);
        response.attempts = context.attempts;
        if (result == ASTERNET_OK || !is_retryable(result) || attempt == retry_limit) break;

        // Bounded exponential backoff with jitter, never exceeding the logical request deadline.
        const int base_delay_ms = std::min(400, 50 << std::min(attempt, 3));
        const int jitter_ms = static_cast<int>(context.request_id % 31);
        const int delay_ms = base_delay_ms + jitter_ms;
        if (monotonic_ms() + delay_ms >= deadline_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    context.request.timeout_ms = initial_timeout;
    return result;
}

int WeakNetInterceptor::intercept(Chain &chain, RequestContext &context,
                                  engine::Response &response) {
    if (prober_ && prober_->is_weak_net()) {
        context.weak_network = true;
        // Do not force H3 or extend caller deadline. Restrict retry amplification under weak links.
        context.max_retries = std::min(context.max_retries, 1);
    }
    return chain.proceed(context, response);
}

struct RequestCoalescer::State {
    struct Flight {
        std::condition_variable ready;
        bool complete = false;
        int result = ASTERNET_ERR_INTERNAL;
        engine::Response response;
        bool dns_cache_hit = false;
    };

    std::mutex mutex;
    size_t max_flights;
    std::unordered_map<std::string, std::shared_ptr<Flight>> flights;
};

RequestCoalescer::RequestCoalescer(size_t max_flights) : state_(std::make_unique<State>()) {
    state_->max_flights = max_flights == 0 ? 1 : max_flights;
}

RequestCoalescer::~RequestCoalescer() = default;

int RequestCoalescer::execute(RequestContext &context, engine::Response &response,
                              const Terminal &operation) {
    if (!is_shareable(context)) return operation(context, response);

    const std::string key = dedup_key(context);
    std::shared_ptr<State::Flight> flight;
    bool leader = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto existing = state_->flights.find(key);
        if (existing == state_->flights.end()) {
            if (state_->flights.size() >= state_->max_flights) {
                return operation(context, response);
            }
            flight = std::make_shared<State::Flight>();
            state_->flights.emplace(key, flight);
            leader = true;
        } else {
            flight = existing->second;
            context.deduplicated = true;
        }
    }

    if (!leader) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        const int64_t deadline_ms = context.deadline_ms > 0
            ? context.deadline_ms : monotonic_ms() + context.request.timeout_ms;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(std::max<int64_t>(0, deadline_ms - monotonic_ms()));
        if (!flight->ready.wait_until(lock, deadline, [&] { return flight->complete; })) {
            response.err_code = ASTERNET_ERR_TIMEOUT;
            response.failure_stage = "dedup_wait";
            return response.err_code;
        }
        response = flight->response;
        context.dns_cache_hit = flight->dns_cache_hit;
        return flight->result;
    }

    int result = ASTERNET_ERR_INTERNAL;
    try {
        result = operation(context, response);
    } catch (...) {
        response = {};
        response.err_code = ASTERNET_ERR_INTERNAL;
        response.failure_stage = "orchestrator";
        result = response.err_code;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        flight->result = result;
        flight->response = response;
        flight->dns_cache_hit = context.dns_cache_hit;
        flight->complete = true;
        state_->flights.erase(key);
    }
    flight->ready.notify_all();
    return result;
}

RequestOrchestrator::RequestOrchestrator(std::shared_ptr<sdt::QualityProber> prober) {
    chain_.add(std::make_unique<WeakNetInterceptor>(std::move(prober)));
    chain_.add(std::make_unique<RetryInterceptor>());
}

int RequestOrchestrator::execute(RequestContext &context, engine::Response &response,
                                 Terminal terminal) {
    normalize_retry_safety(context);
    return coalescer_.execute(context, response, [this, &terminal](RequestContext &coalesced_context,
                                                                      engine::Response &coalesced_response) {
        return chain_.execute(coalesced_context, coalesced_response, terminal);
    });
}

}  // namespace orchestrator
}  // namespace asternet
