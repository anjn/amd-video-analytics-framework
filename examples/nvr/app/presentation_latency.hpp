#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <stdexcept>

namespace amdva::nvr::detail {
// GUI-thread metadata only. Samples end at compositor presentation time, not
// callback receipt or swap. No image access or GPU synchronization is involved.
class PresentationLatency {
  public:
    struct Stats {
        size_t samples{};
        double mean_ms{}, maximum_ms{};
        bool available{};
    };
    explicit PresentationLatency(int64_t window_ns = 10000000000LL, size_t capacity = 512)
        : window_ns_(window_ns), capacity_(capacity) {
        if (window_ns <= 0 || !capacity || capacity > 512)
            throw std::invalid_argument("Invalid presentation latency bounds");
    }
    bool observe(uint64_t generation, uint64_t sequence, int64_t input_ns,
                 int64_t presented_ns, int64_t observed_ns) {
        if (!generation || !sequence || generation < generation_)
            return false;
        if (generation != generation_) {
            generation_ = generation;
            sequence_ = 0;
            presented_ns_ = 0;
            head_ = count_ = max_head_ = max_count_ = 0; sum_ = 0;
            incomplete_ = overflowed_ = false;
        }
        if (sequence <= sequence_)
            return false; // Repeated/stale feedback is not another sample.
        if (input_ns <= 0 || presented_ns < input_ns ||
            presented_ns < presented_ns_) {
            incomplete_ = true; incomplete_at_ = observed_ns;
            return false;
        }
        sequence_ = sequence;
        presented_ns_ = presented_ns;
        prune(observed_ns);
        if (presented_ns < observed_ns - window_ns_) {
            incomplete_ = true; incomplete_at_ = observed_ns;
            return false;
        }
        if (count_ == capacity_) {
            overflowed_ = true; overflow_at_ = samples_[head_].at;
            remove_front();
        }
        // Window membership follows feedback receipt. The latency endpoint
        // remains the reported presentation time, which can be slightly ahead
        // of receipt (the compositor may account for the physical display path).
        Sample sample{observed_ns, presented_ns - input_ns, sequence};
        sum_ += sample.latency;
        samples_[(head_ + count_) % capacity_] = sample;
        ++count_;
        while (max_count_ && maxima_[(max_head_ + max_count_ - 1) % capacity_].latency <= sample.latency)
            --max_count_;
        maxima_[(max_head_ + max_count_) % capacity_] = sample;
        ++max_count_;
        return true;
    }
    Stats snapshot(uint64_t generation, int64_t now_ns) {
        if (!generation || generation != generation_)
            return {};
        prune(now_ns);
        Stats result;
        result.samples = count_;
        result.available = count_ != 0 &&
            !(incomplete_ && now_ns - incomplete_at_ <= window_ns_) &&
            !(overflowed_ && now_ns - overflow_at_ <= window_ns_);
        if (result.available) {
            result.mean_ms = double(sum_ / count_) / 1e6;
            result.maximum_ms = double(maxima_[max_head_].latency) / 1e6;
        }
        return result;
    }
  private:
    struct Sample { int64_t at, latency; uint64_t sequence; };
    int64_t window_ns_, presented_ns_{}, incomplete_at_{}, overflow_at_{};
    size_t capacity_;
    uint64_t generation_{}, sequence_{};
    // Fixed rings avoid allocations in the presentation callback/slot lock.
    // 512 samples cover the supported 30fps × 10s window with headroom.
    std::array<Sample,512> samples_{}, maxima_{};
    size_t head_{}, count_{}, max_head_{}, max_count_{};
    long double sum_{};
    bool incomplete_{}, overflowed_{};
    void remove_front() {
        const auto sample = samples_[head_];
        sum_ -= sample.latency;
        if (max_count_ && maxima_[max_head_].sequence == sample.sequence) {
            max_head_ = (max_head_ + 1) % capacity_;
            --max_count_;
        }
        head_ = (head_ + 1) % capacity_;
        --count_;
    }
    void prune(int64_t now_ns) {
        while (count_ != 0 && samples_[head_].at < now_ns - window_ns_)
            remove_front();
    }
};
}
