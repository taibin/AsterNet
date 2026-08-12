#include "sdt/quality_prober.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <utility>

namespace asternet {
namespace sdt {

int compute_score(const QualitySample &sample) {
    if (sample.rtt_ms < 0 && sample.loss_permil < 0 && sample.bandwidth_kbps < 0) return -1;

    int score = 100;
    if (sample.rtt_ms >= 0) {
        if (sample.rtt_ms >= 2000) score -= 70;
        else if (sample.rtt_ms >= 1000) score -= 50;
        else if (sample.rtt_ms >= 500) score -= 35;
        else if (sample.rtt_ms >= 300) score -= 20;
        else if (sample.rtt_ms >= 100) score -= 10;
    }
    if (sample.loss_permil >= 0) score -= std::min(30, sample.loss_permil / 2);
    if (sample.bandwidth_kbps >= 0) {
        if (sample.bandwidth_kbps < 100) score -= 20;
        else if (sample.bandwidth_kbps < 500) score -= 10;
    }
    return std::max(0, std::min(100, score));
}

struct QualityProberImpl::State {
    explicit State(Config cfg, ProbeCallback callback)
        : config(std::move(cfg)), probe_callback(std::move(callback)) {}

    Config config;
    mutable std::mutex mutex;
    ProbeCallback probe_callback;
    QualitySnapshot snapshot;
    size_t consecutive_good_samples = 0;
};

QualityProberImpl::QualityProberImpl() : QualityProberImpl(Config{}, {}) {}

QualityProberImpl::QualityProberImpl(Config config, ProbeCallback probe_callback)
    : state_(std::make_unique<State>(std::move(config), std::move(probe_callback))) {}

QualityProberImpl::~QualityProberImpl() = default;

int QualityProberImpl::probe() {
    ProbeCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        callback = state_->probe_callback;
    }
    if (!callback) return current_score();
    const QualitySample sample = callback();
    observe_sample(sample.rtt_ms >= 0 || sample.loss_permil >= 0 || sample.bandwidth_kbps >= 0,
                   sample);
    return current_score();
}

void QualityProberImpl::observe(bool success, int rtt_ms) {
    QualitySample sample;
    sample.rtt_ms = rtt_ms;
    observe_sample(success, sample);
}

void QualityProberImpl::observe_sample(bool success, const QualitySample &sample) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    QualitySnapshot &current = state_->snapshot;
    if (!success) {
        ++current.consecutive_failures;
        state_->consecutive_good_samples = 0;
        if (current.consecutive_failures >= state_->config.failures_before_bad) {
            current.quality = NetworkQuality::kBad;
            current.score = 0;
        } else if (current.quality == NetworkQuality::kGood) {
            current.quality = NetworkQuality::kDegraded;
        }
        return;
    }

    current.consecutive_failures = 0;
    ++state_->consecutive_good_samples;
    ++current.samples;
    if (sample.rtt_ms >= 0) {
        current.smoothed_rtt_ms = current.smoothed_rtt_ms < 0
            ? sample.rtt_ms
            : (current.smoothed_rtt_ms * 7 + sample.rtt_ms * 3) / 10;
    }
    if (sample.bandwidth_kbps >= 0) current.bandwidth_kbps = sample.bandwidth_kbps;

    QualitySample aggregated = sample;
    if (current.smoothed_rtt_ms >= 0) aggregated.rtt_ms = current.smoothed_rtt_ms;
    if (current.bandwidth_kbps >= 0) aggregated.bandwidth_kbps = current.bandwidth_kbps;
    const int score = compute_score(aggregated);
    if (score >= 0) current.score = score;

    if (current.score < 0) {
        current.quality = NetworkQuality::kUnknown;
    } else if (current.score < state_->config.weak_score_threshold) {
        current.quality = NetworkQuality::kBad;
        state_->consecutive_good_samples = 0;
    } else if (current.score < state_->config.degraded_score_threshold) {
        current.quality = NetworkQuality::kDegraded;
        state_->consecutive_good_samples = 0;
    } else if (state_->consecutive_good_samples >= state_->config.good_samples_to_recover
               || current.quality == NetworkQuality::kUnknown) {
        current.quality = NetworkQuality::kGood;
    }
}

int QualityProberImpl::current_score() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->snapshot.score;
}

bool QualityProberImpl::is_weak_net() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->snapshot.quality == NetworkQuality::kDegraded
        || state_->snapshot.quality == NetworkQuality::kBad
        || state_->snapshot.quality == NetworkQuality::kOffline;
}

QualitySnapshot QualityProberImpl::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->snapshot;
}

void QualityProberImpl::on_network_change(uint64_t network_epoch, asternet_network_t /*net*/) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->snapshot = {};
    state_->snapshot.network_epoch = network_epoch;
    state_->consecutive_good_samples = 0;
}

std::string QualityProberImpl::dump() const {
    const QualitySnapshot current = snapshot();
    std::ostringstream out;
    out << "{\"score\":" << current.score << ",\"quality\":"
        << static_cast<int>(current.quality) << ",\"samples\":" << current.samples
        << ",\"failures\":" << current.consecutive_failures << ",\"network_epoch\":"
        << current.network_epoch << "}";
    return out.str();
}

void QualityProberImpl::set_probe_callback(ProbeCallback probe_callback) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->probe_callback = std::move(probe_callback);
}

}  // namespace sdt
}  // namespace asternet
