#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace amdva::nvr {
struct AiMetric {
    std::string stage, camera;
    unsigned samples{}, pending{};
    uint64_t completed{};
    double mean_ms{}, p50_ms{}, p95_ms{}, max_ms{}, wait_ms{}, rate{}, mean_batch{};
    uint64_t dropped{};
    bool truncated{};
};
// Completed wall-clock measurements. These are not GPU utilization counters.
class AiRuntime {
  public:
    using Clock = std::chrono::steady_clock;
    void record(const std::string &stage, const std::string &camera, double ms, double wait = 0,
                unsigned batch = 1);
    void queue(const std::string &stage, const std::string &camera, unsigned pending,
               uint64_t drops = 0);
    std::vector<AiMetric> snapshot() const;
    void reset_measurements();
    void configure(double fps, unsigned interval_ms);
    double fps() const;
    unsigned interval_ms() const;

  private:
    struct Sample {
        Clock::time_point at;
        double ms, wait;
        unsigned batch;
    };
    struct Row {
        uint64_t completed{};
        std::deque<Sample> samples;
        unsigned pending{};
        uint64_t dropped{};
        bool truncated{};
    };
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Row> rows_;
    Clock::time_point started_ = Clock::now();
    double fps_{2};
    unsigned interval_{5000};
};
} // namespace amdva::nvr
