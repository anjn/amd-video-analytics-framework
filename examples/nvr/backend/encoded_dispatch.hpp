#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace amdva::nvr {
// One AU producer. Running dispatch uses the receive thread, never a storage
// worker. The bounded queue is only for decoder startup/rebinding, not a
// post-decode FIFO. Lifecycle callers must pause before destroying the sink.
template<class T> class EncodedDispatch {
    using Clock = std::chrono::steady_clock;
    struct Entry { T value; size_t bytes; Clock::time_point at; };
    size_t limit_, bytes_{};
    std::function<size_t(const T&)> size_;
    std::function<bool(const T&)> key_;
    std::function<uint64_t(const T&)> order_;
    std::function<void(const T&)> preroll_;
    std::function<void(const T&)> submit_;
    std::deque<Entry> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool enabled_{}, needs_key_{true};
    uint64_t last_seen_{};
    std::string error_;
    void submit(const T& value) {
        const auto order=order_(value);
        if (!order) throw std::runtime_error("Missing ingest sample sequence");
        if (order<=last_seen_) return; // Bootstrap seed echoed by the receive callback.
        if (needs_key_ && !key_(value)) {
            if (preroll_) preroll_(value);
            last_seen_=order;
            return;
        }
        needs_key_ = false;
        try { submit_(value); last_seen_=order; }
        catch (const std::exception& e) { error_ = e.what(); throw; }
        catch (...) { error_ = "Unknown decode dispatch failure"; throw; }
    }
  public:
    struct Snapshot { size_t frames{}, bytes{}; int64_t oldest_ns{}; bool running{}; };
    EncodedDispatch(size_t limit, std::function<size_t(const T&)> size,
                    std::function<bool(const T&)> key,
                    std::function<uint64_t(const T&)> order,
                    std::function<void(const T&)> preroll = {})
        : limit_(limit), size_(std::move(size)), key_(std::move(key)), order_(std::move(order)), preroll_(std::move(preroll)) {
        if (!limit_) throw std::invalid_argument("Decode dispatch capacity is zero");
    }
    void enable() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) { enabled_ = true; needs_key_ = true; error_.clear(); }
    }
    bool receive(const T& value) {
        const auto bytes = size_(value);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!enabled_) return false;
        if (bytes > limit_) {
            error_ = "Encoded AU exceeds decode dispatch capacity";
            throw std::runtime_error(error_);
        }
        // Backpressure, never overwrite/drop a queued AU to make room.
        cv_.wait(lock,[&] { return !enabled_ || submit_ || !error_.empty() || bytes <= limit_-bytes_; });
        if (!enabled_) return false;
        if (!error_.empty()) throw std::runtime_error(error_);
        if (submit_) submit(value);
        else { queue_.push_back({value,bytes,Clock::now()}); bytes_ += bytes; }
        return true;
    }
    void activate(std::function<void(const T&)> sink, const T& seed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) throw std::runtime_error("Decode dispatch is disabled");
        if (!error_.empty()) throw std::runtime_error(error_);
        if (!sink || submit_) throw std::logic_error("Decode dispatch needs one quiesced sink");
        const auto first=order_(seed);
        if(!first) throw std::runtime_error("Missing decoder bootstrap identity");
        if(size_(seed)>limit_) throw std::runtime_error("Decoder bootstrap exceeds bounded capacity");
        while(!queue_.empty() && order_(queue_.front().value)<first) {
            if(order_(queue_.front().value)>last_seen_ && preroll_) preroll_(queue_.front().value);
            last_seen_=std::max(last_seen_,order_(queue_.front().value));
            bytes_-=queue_.front().bytes;queue_.pop_front();
        }
        // The recording consumer may observe the seed before on_sample runs.
        // Seed it once, then suppress that callback's echo using the receive ID.
        const bool seed_missing=first>last_seen_ && (queue_.empty() || order_(queue_.front().value)!=first);
        submit_ = std::move(sink); needs_key_ = true;
        try {
            if(seed_missing) submit(seed);
            while (!queue_.empty()) {
                submit(queue_.front().value);
                bytes_ -= queue_.front().bytes;
                queue_.pop_front();
            }
        } catch (...) { cv_.notify_all(); throw; }
        cv_.notify_all();
    }
    void pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        submit_ = {}; needs_key_ = true;
    }
    std::vector<T> close() {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false; submit_ = {}; needs_key_ = true; last_seen_=0;
        std::vector<T> retired;
        for (auto& entry : queue_) retired.push_back(std::move(entry.value));
        queue_.clear(); bytes_ = 0; cv_.notify_all();
        return retired;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {queue_.size(),bytes_,queue_.empty()?0:
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-queue_.front().at).count(),bool(submit_)};
    }
};
}
