#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace amdva::nvr {
// One decoded-frame producer and one preview consumer per generation. Holds
// sample references, never pixels. Full queues backpressure for a bounded time;
// strict callers fail on pressure; live callers explicitly account dropped IDs.
template<class T> class PresentationFifo {
  public:
    struct Entry {
        T frame;
        uint64_t generation{}, serial{}, pts{};
        int64_t entered_ns{}, due_ns{}, period_ns{};
    };
    enum class Result { Queued, Inactive, Retired };
    enum class Policy { Strict, DropOldest };
    enum class DropReason { Full, Late };
    struct Dropped { Entry entry; DropReason reason; };
    struct Snapshot {
        size_t frames{}, capacity{};
        uint64_t generation{}, full_waits{};
        int64_t oldest_ns{}, next_due_ns{};
        bool active{}, closed{};
        std::string error;
        uint64_t full_dropped{},late_dropped{},late_recoveries{},clock_resyncs{};
    };
  private:
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Entry> queue_;
    size_t capacity_, prefill_;
    std::chrono::milliseconds wait_;
    std::function<int64_t()> now_;
    uint64_t epoch_{}, generation_{}, first_pts_{}, last_pts_{}, last_serial_{}, full_waits_{};
    int64_t anchor_ns_{}, period_ns_{};
    bool active_{}, closed_{true}, anchored_{};
    std::string error_;
    Policy policy_;
    uint64_t full_dropped_{},late_dropped_{},late_recoveries_{},clock_resyncs_{};
    void drop_front(std::vector<Dropped> &dropped,DropReason reason) {
        dropped.push_back({std::move(queue_.front()),reason});queue_.pop_front();
        if(reason==DropReason::Full)++full_dropped_;else ++late_dropped_;
    }
    [[noreturn]] void fail(const std::string &message) {
        error_=message;available_.notify_all();throw std::runtime_error(message);
    }
    std::vector<Entry> retire() {
        std::vector<Entry> result;
        while(!queue_.empty()){result.push_back(std::move(queue_.front()));queue_.pop_front();}
        anchored_=false;available_.notify_all();return result;
    }
  public:
    PresentationFifo(size_t capacity=3,size_t prefill=2,
                     std::chrono::milliseconds wait=std::chrono::milliseconds(100),
                     std::function<int64_t()> now=[] {
                         return std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
                     },Policy policy=Policy::Strict)
        :capacity_(capacity),prefill_(prefill),wait_(wait),now_(std::move(now)),policy_(policy) {
        if(!capacity_ || prefill_>=capacity_ || wait_.count()<=0)
            throw std::invalid_argument("Invalid presentation FIFO bounds");
    }
    std::vector<Entry> reset(uint64_t generation,bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++epoch_;auto result=retire();generation_=generation;active_=active;closed_=false;
        error_.clear();full_waits_=0;full_dropped_=late_dropped_=late_recoveries_=clock_resyncs_=0;last_serial_=0;return result;
    }
    std::vector<Entry> active(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(value==active_)return {};
        ++epoch_;active_=value;return value?std::vector<Entry>{}:retire();
    }
    std::vector<Entry> close() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++epoch_;closed_=true;active_=false;return retire();
    }
    Result push(T frame,uint64_t generation,uint64_t serial,uint64_t pts,int64_t period_ns,
                std::vector<Dropped> *discarded=nullptr) {
        // Destruction of retired samples happens after unlocking the FIFO.
        std::vector<Dropped> local;
        auto &dropped=discarded?*discarded:local;
        std::unique_lock<std::mutex> lock(mutex_);
        if(closed_ || generation!=generation_)return Result::Retired;
        if(!active_)return Result::Inactive;
        if(!error_.empty())throw std::runtime_error(error_);
        const auto epoch=epoch_;
        if(queue_.size()==capacity_ && policy_==Policy::Strict) {
            ++full_waits_;
            if(!available_.wait_for(lock,wait_,[&] {
                return closed_ || !active_ || generation!=generation_ || epoch!=epoch_ ||
                       !error_.empty() || queue_.size()<capacity_;
            }))fail("Presentation FIFO remained full; no frame was overwritten");
        }
        if(closed_ || generation!=generation_ || epoch!=epoch_)return Result::Retired;
        if(!active_)return Result::Inactive;
        if(!error_.empty())throw std::runtime_error(error_);
        if(!serial || pts==std::numeric_limits<uint64_t>::max() ||
           period_ns<=0 || period_ns>1000000000LL)
            fail("Presentation FIFO needs a valid identity, PTS and frame rate");
        if(anchored_ && (serial<=last_serial_ || pts<=last_pts_))
            fail("Presentation FIFO input identity or PTS did not increase");
        if(anchored_ && period_ns!=period_ns_)
            fail("Presentation FIFO frame rate changed without a new generation");
        const auto entered=now_();
        if(!anchored_) {
            first_pts_=pts;anchor_ns_=entered;period_ns_=period_ns;anchored_=true;
        }
        const auto span=pts-first_pts_;
        const auto maximum=std::numeric_limits<int64_t>::max();
        if(anchor_ns_<0 || span>uint64_t(maximum-anchor_ns_) ||
           prefill_>uint64_t(maximum-anchor_ns_-int64_t(span))/uint64_t(period_ns))
            fail("Presentation FIFO timestamp overflow");
        auto due=anchor_ns_+int64_t(prefill_)*period_ns+int64_t(span);
        if(policy_==Policy::DropOldest) {
            if(prefill_>uint64_t(maximum-entered)/uint64_t(period_ns))
                fail("Presentation FIFO timestamp overflow");
            const auto latest_due=entered+int64_t(prefill_)*period_ns;
            if(due>latest_due) {
                const auto shift=due-latest_due;
                if(shift>anchor_ns_)fail("Presentation FIFO PTS clock exceeds monotonic bounds");
                anchor_ns_-=shift;
                for(auto &entry:queue_)entry.due_ns-=shift;
                due=latest_due;++clock_resyncs_;
            }
        }
        // A slowly deficient consumer can pop within every full-wait timeout
        // while latency still grows without bound. Never hide that backlog.
        if(entered>due && uint64_t(entered-due)>uint64_t(period_ns)*4) {
            if(policy_==Policy::Strict)
                fail("Presentation FIFO exceeded four frame periods of lateness; no frame was overwritten");
            while(!queue_.empty())drop_front(dropped,DropReason::Late);
            // Catch up immediately. Do not move the PTS clock forward, which
            // would permanently add a transient decoder backlog to live latency.
            due=entered;
            ++late_recoveries_;
        }
        if(queue_.size()==capacity_)drop_front(dropped,DropReason::Full);
        queue_.push_back({std::move(frame),generation,serial,pts,entered,due,period_ns});
        last_pts_=pts;last_serial_=serial;return Result::Queued;
    }
    std::optional<Entry> peek() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty())return {};
        return queue_.front();
    }
    std::optional<Entry> pop(uint64_t generation,uint64_t serial) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty() || queue_.front().generation!=generation ||
           queue_.front().serial!=serial)return {};
        auto result=std::move(queue_.front());queue_.pop_front();
        available_.notify_one();return result;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {queue_.size(),capacity_,generation_,full_waits_,
                queue_.empty()?0:now_()-queue_.front().entered_ns,
                queue_.empty()?0:queue_.front().due_ns,active_,closed_,error_,full_dropped_,late_dropped_,late_recoveries_,clock_resyncs_};
    }
};
} // namespace amdva::nvr
