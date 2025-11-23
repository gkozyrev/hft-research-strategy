#include "strategy/latency_tracker.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace strategy {

LatencyTracker::LatencyTracker(size_t max_samples)
    : max_samples_(max_samples) {
}

void LatencyTracker::record(double latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    samples_.push_back(latency_ms);
    
    // Keep only the most recent samples
    if (samples_.size() > max_samples_) {
        samples_.pop_front();
    }
}

LatencyStats LatencyTracker::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (samples_.empty()) {
        return LatencyStats{};
    }
    
    std::vector<double> sorted_samples(samples_.begin(), samples_.end());
    std::sort(sorted_samples.begin(), sorted_samples.end());
    
    return calculate_stats(sorted_samples);
}

LatencyStats LatencyTracker::get_stats(const std::string& label) const {
    // For now, we only support one set of samples
    // Could be extended to support labeled tracking
    (void)label;
    return get_stats();
}

void LatencyTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

void LatencyTracker::reset(const std::string& label) {
    (void)label;
    reset();
}

std::string LatencyTracker::format_stats() const {
    auto stats = get_stats();
    
    if (stats.count == 0) {
        return "No samples";
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "min=" << stats.min_ms << "ms"
        << " avg=" << stats.avg_ms << "ms"
        << " max=" << stats.max_ms << "ms"
        << " p50=" << stats.p50_ms << "ms"
        << " p95=" << stats.p95_ms << "ms"
        << " p99=" << stats.p99_ms << "ms"
        << " (n=" << stats.count << ")";
    
    return oss.str();
}

std::string LatencyTracker::format_stats(const std::string& label) const {
    auto stats = get_stats(label);
    
    if (stats.count == 0) {
        return label + ": No samples";
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << label << ": "
        << "min=" << stats.min_ms << "ms"
        << " avg=" << stats.avg_ms << "ms"
        << " max=" << stats.max_ms << "ms"
        << " p50=" << stats.p50_ms << "ms"
        << " p95=" << stats.p95_ms << "ms"
        << " p99=" << stats.p99_ms << "ms"
        << " (n=" << stats.count << ")";
    
    return oss.str();
}

LatencyStats LatencyTracker::calculate_stats(const std::vector<double>& sorted_samples) const {
    LatencyStats stats;
    stats.count = sorted_samples.size();
    
    if (stats.count == 0) {
        return stats;
    }
    
    stats.min_ms = sorted_samples.front();
    stats.max_ms = sorted_samples.back();
    
    // Calculate average
    double sum = std::accumulate(sorted_samples.begin(), sorted_samples.end(), 0.0);
    stats.avg_ms = sum / stats.count;
    
    // Calculate percentiles
    if (stats.count > 0) {
        auto p50_idx = static_cast<size_t>(stats.count * 0.50);
        auto p95_idx = static_cast<size_t>(stats.count * 0.95);
        auto p99_idx = static_cast<size_t>(stats.count * 0.99);
        
        p50_idx = std::min(p50_idx, stats.count - 1);
        p95_idx = std::min(p95_idx, stats.count - 1);
        p99_idx = std::min(p99_idx, stats.count - 1);
        
        stats.p50_ms = sorted_samples[p50_idx];
        stats.p95_ms = sorted_samples[p95_idx];
        stats.p99_ms = sorted_samples[p99_idx];
    }
    
    return stats;
}

} // namespace strategy

