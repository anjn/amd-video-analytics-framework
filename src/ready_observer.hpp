#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace amdva::detail {
// Metadata-only readiness queries. Never run GPU completion waits on a producer.
class ReadyObserver {
public:
    enum class State { Busy, Ready, Error };
    using Active = std::shared_ptr<std::atomic<bool>>;
    using Query = std::function<State()>;
    using Done = std::function<void(bool, int64_t, int64_t)>;
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    explicit ReadyObserver(size_t capacity=64,
                           std::chrono::milliseconds timeout=std::chrono::seconds(1))
        : capacity_(capacity), timeout_ns_(timeout.count()*1000000),
          thread_([this] { run(); }) {}
    ~ReadyObserver() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
        changed_.notify_all();
        thread_.join();
    }
    bool submit(Active active, Query query, Done done, int64_t output_ns) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || outstanding_>=capacity_) return false;
        jobs_.push_back({std::move(active),std::move(query),std::move(done),output_ns,output_ns});
        ++outstanding_; changed_.notify_one(); return true;
    }
private:
    struct Job { Active active; Query query; Done done; int64_t submitted,previous; };
    size_t capacity_,outstanding_{};
    int64_t timeout_ns_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_{};
    std::vector<Job> jobs_;
    std::thread thread_;
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for(;;) {
            changed_.wait(lock,[&] { return stopping_ || !jobs_.empty(); });
            if(stopping_) { jobs_.clear(); return; }
            std::vector<Job> batch,pending; batch.swap(jobs_);
            lock.unlock();
            size_t finished=0;
            for(auto &job:batch) {
                if(!job.active->load()) { ++finished; continue; }
                const auto before=now();
                State state=State::Error;
                if(before-job.submitted<timeout_ns_) {
                    try { state=job.query(); } catch(...) { state=State::Error; }
                }
                const auto observed=now();
                if(state==State::Busy && observed-job.submitted<timeout_ns_) {
                    job.previous=before; pending.push_back(std::move(job));
                } else {
                    if(job.active->load()) job.done(state==State::Ready,observed,observed-job.previous);
                    ++finished;
                }
            }
            lock.lock();
            outstanding_-=finished;
            for(auto &job:pending) jobs_.push_back(std::move(job));
            // A query does not wait for GPU completion. Bound the polling rate.
            if(!jobs_.empty()) changed_.wait_for(lock,std::chrono::milliseconds(1),
                                                [&] { return stopping_; });
        }
    }
};
} // namespace amdva::detail
