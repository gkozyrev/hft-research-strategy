#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace strategy {

struct LatencyStats {
    double min_ms = 0.0;
    double max_ms = 0.0;
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    size_t count = 0;
};

class LatencyTracker {
public:
    explicit LatencyTracker(size_t max_samples = 1000);
    
    // Record a latency measurement in milliseconds
    void record(double latency_ms);
    
    // Record a latency measurement from time points
    template<typename TimePoint>
    void record(TimePoint start, TimePoint end) {
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        record(duration.count() / 1000.0);
    }
    
    // Get current statistics
    LatencyStats get_stats() const;
    
    // Get statistics for a specific label (if using labeled tracking)
    LatencyStats get_stats(const std::string& label) const;
    
    // Reset all measurements
    void reset();
    
    // Reset measurements for a specific label
    void reset(const std::string& label);
    
    // Format stats as string for display
    std::string format_stats() const;
    std::string format_stats(const std::string& label) const;

private:
    mutable std::mutex mutex_;
    std::deque<double> samples_;
    size_t max_samples_;
    
    LatencyStats calculate_stats(const std::vector<double>& sorted_samples) const;
};

} // namespace strategy

