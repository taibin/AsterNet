#include "sdt/quality_prober.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <utility>

namespace asternet {
namespace sdt {

namespace {

thread_local int g_quality_callback_suppression_depth = 0;

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void update_loss(QualitySnapshot &snapshot) {
    snapshot.loss_permil = static_cast<int>(std::min<size_t>(1000,
        snapshot.total_failures * 1000 / std::max<size_t>(1, snapshot.samples)));
}

bool has_metrics(const QualitySample &sample) {
    return sample.rtt_ms >= 0 || sample.loss_permil >= 0 || sample.bandwidth_kbps >= 0;
}

}  // namespace

QualityCallbackSuppressionGuard::QualityCallbackSuppressionGuard() {
    ++g_quality_callback_suppression_depth;
}

QualityCallbackSuppressionGuard::~QualityCallbackSuppressionGuard() {
    --g_quality_callback_suppression_depth;
}

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
    QualityProberImpl::QualityChangeCallback quality_change_callback;
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
        if (callback) {
            ++state_->snapshot.probe_count;
            state_->snapshot.last_probe_ms = monotonic_ms();
        }
    }
    if (!callback) return current_score();
    const QualitySample sample = callback();
    observe_sample(has_metrics(sample), sample);
    return current_score();
}

void QualityProberImpl::observe(bool success, int rtt_ms) {
    QualitySample sample;
    sample.rtt_ms = rtt_ms;
    observe_sample(success, sample);
}

void QualityProberImpl::observe_sample(bool success, const QualitySample &sample) {
    QualityChangeCallback callback;
    QualitySnapshot snapshot;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        QualitySnapshot &current = state_->snapshot;
        const QualitySnapshot before = current;
        const int64_t now = monotonic_ms();

        current.samples += 1;
        current.last_sample_ms = now;
        if (sample.net != ASTERNET_NETWORK_UNKNOWN) current.network = sample.net;

        if (!success) {
            ++current.failure_samples;
            ++current.total_failures;
            ++current.consecutive_failures;
            current.last_failure_ms = now;
            state_->consecutive_good_samples = 0;
            update_loss(current);

            if (current.network == ASTERNET_NETWORK_NONE) {
                current.quality = NetworkQuality::kOffline;
                current.score = 0;
            } else if (current.consecutive_failures >= state_->config.failures_before_bad) {
                current.quality = NetworkQuality::kBad;
                current.score = 0;
            } else if (before.quality == NetworkQuality::kGood) {
                current.quality = NetworkQuality::kDegraded;
                current.score = 0;
            } else {
                current.score = 0;
            }
        } else {
            ++current.success_samples;
            current.last_success_ms = now;
            current.consecutive_failures = 0;

            if (sample.rtt_ms >= 0) {
                current.smoothed_rtt_ms = current.smoothed_rtt_ms < 0
                    ? sample.rtt_ms
                    : (current.smoothed_rtt_ms * 7 + sample.rtt_ms * 3) / 10;
            }
            if (sample.bandwidth_kbps >= 0) current.bandwidth_kbps = sample.bandwidth_kbps;

            QualitySample aggregated = sample;
            if (current.smoothed_rtt_ms >= 0) aggregated.rtt_ms = current.smoothed_rtt_ms;
            if (current.loss_permil >= 0) aggregated.loss_permil = current.loss_permil;
            if (current.bandwidth_kbps >= 0) aggregated.bandwidth_kbps = current.bandwidth_kbps;

            const int score = compute_score(aggregated);
            if (score >= 0) current.score = score;

            if (current.network == ASTERNET_NETWORK_NONE) {
                current.quality = NetworkQuality::kOffline;
                current.score = 0;
                state_->consecutive_good_samples = 0;
            } else if (current.score >= 0) {
                if (current.score < state_->config.weak_score_threshold) {
                    current.quality = NetworkQuality::kBad;
                    state_->consecutive_good_samples = 0;
                } else if (current.score < state_->config.degraded_score_threshold) {
                    current.quality = NetworkQuality::kDegraded;
                    state_->consecutive_good_samples = 0;
                } else {
                    ++state_->consecutive_good_samples;
                    if (before.quality == NetworkQuality::kUnknown
                        || before.quality == NetworkQuality::kGood
                        || state_->consecutive_good_samples >= state_->config.good_samples_to_recover) {
                        current.quality = NetworkQuality::kGood;
                    }
                }
            }
        }

        update_loss(current);
        changed = current.quality != before.quality || current.network != before.network;
        if (changed) {
            current.last_quality_change_ms = now;
            callback = state_->quality_change_callback;
            snapshot = current;
        }
    }

    if (callback && g_quality_callback_suppression_depth == 0) callback(snapshot);
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

void QualityProberImpl::on_network_change(uint64_t network_epoch, asternet_network_t net) {
    QualityChangeCallback callback;
    QualitySnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const QualitySnapshot before = state_->snapshot;
        state_->snapshot = {};
        state_->snapshot.network_epoch = network_epoch;
        state_->snapshot.network = net;
        state_->consecutive_good_samples = 0;
        if (net == ASTERNET_NETWORK_NONE) {
            state_->snapshot.quality = NetworkQuality::kOffline;
            state_->snapshot.score = 0;
        } else {
            state_->snapshot.quality = NetworkQuality::kUnknown;
        }
        state_->snapshot.last_quality_change_ms = monotonic_ms();
        callback = state_->quality_change_callback;
        snapshot = state_->snapshot;
        if (before.quality == snapshot.quality && before.network == snapshot.network
            && before.network_epoch == snapshot.network_epoch) {
            callback = nullptr;
        }
    }

    if (callback && g_quality_callback_suppression_depth == 0) callback(snapshot);
}

void QualityProberImpl::set_quality_change_callback(QualityChangeCallback callback) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->quality_change_callback = std::move(callback);
}

std::string QualityProberImpl::dump() const {
    const QualitySnapshot current = snapshot();
    std::ostringstream out;
    out << "{\"score\":" << current.score << ",\"quality\":"
        << static_cast<int>(current.quality) << ",\"samples\":" << current.samples
        << ",\"probe_count\":" << current.probe_count << ",\"success_samples\":"
        << current.success_samples << ",\"failure_samples\":" << current.failure_samples
        << ",\"failures\":" << current.consecutive_failures << ",\"total_failures\":"
        << current.total_failures << ",\"loss_permil\":" << current.loss_permil
        << ",\"smoothed_rtt_ms\":" << current.smoothed_rtt_ms
        << ",\"last_probe_ms\":" << current.last_probe_ms
        << ",\"last_success_ms\":" << current.last_success_ms
        << ",\"last_failure_ms\":" << current.last_failure_ms
        << ",\"last_quality_change_ms\":" << current.last_quality_change_ms
        << ",\"last_sample_ms\":" << current.last_sample_ms
        << ",\"network\":" << static_cast<int>(current.network)
        << ",\"network_epoch\":" << current.network_epoch << "}";
    return out.str();
}

void QualityProberImpl::set_probe_callback(ProbeCallback probe_callback) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->probe_callback = std::move(probe_callback);
}

}  // namespace sdt
}  // namespace asternet
