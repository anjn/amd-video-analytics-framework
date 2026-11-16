#include "../src/ready_observer.hpp"
#include "amdva/telemetry.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
using namespace amdva;
void check(bool yes,const char *message) { if(!yes) throw std::runtime_error(message); }
int main() {
    try {
        Telemetry disabled("off",{false,1000});
        disabled.counter(TimingCounter::PresentationDropped);
        check(disabled.counter_value(TimingCounter::PresentationDropped)==1 &&
              disabled.snapshot().presentation_dropped==1,
              "Live drop accounting remains active with timing disabled");
        int64_t clock=1000000000;
        Telemetry t("cam",{},[&] { return clock; });
        auto frame=t.frame(clock);
        clock+=100000;t.mark(frame,TimingStage::DecodeInput);
        clock+=200000;t.mark(frame,TimingStage::DecodeOutput);
        clock+=100000;t.mark(frame,TimingStage::PreviewReady);
        clock+=100000;t.mark(frame,TimingStage::Draw);
        clock+=100000;t.mark(frame,TimingStage::Swap);
        clock+=100000;
        check(t.decode_ready(frame,clock,100000),"Ready may be observed after draw/swap");
        check(!t.decode_ready(frame,clock,100000),"Completion must be counted once");
        auto s=t.snapshot();
        check(s.latency[0][0].mean_ms==0.3,"Notification latency retained");
        check(s.latency[0][input_decode_ready_latency].mean_ms==0.7,"GPU-ready observation latency");
        check(s.decode_ready_uncertainty[0].maximum_ms==0.1,"Observation uncertainty retained");
        t.decode_ready_missing(frame);
        check(!t.snapshot().decode_ready_complete[0],"Missing observation invalidates recent coverage");
        clock+=10000000001LL;
        check(t.snapshot().decode_ready_complete[0],"Missing coverage ages out with window");
        auto no_input=t.frame();
        clock+=100000;t.mark(no_input,TimingStage::DecodeInput);
        clock+=100000;t.mark(no_input,TimingStage::DecodeOutput);
        clock+=100000;
        check(t.decode_ready(no_input,clock,100000),"GPU readiness without input can still be observed");
        check(!t.snapshot().decode_ready_complete[0],"Missing input identity invalidates input latency");
        auto old=frame;t.begin_generation();
        check(!t.decode_ready(old,clock,0),"Old generation cannot contaminate new measurements");
        auto missing=t.frame(clock);
        check(!t.decode_ready(missing,clock,0),"Cannot invent completion without decoder output");

        using Observer=detail::ReadyObserver;
        auto active=std::make_shared<std::atomic<bool>>(true);
        auto ready=std::make_shared<std::atomic<bool>>(false);
        std::promise<bool> promise;auto future=promise.get_future();
        Observer observer(1,std::chrono::milliseconds(100));
        check(observer.submit(active,[ready] { return ready->load()?Observer::State::Ready:Observer::State::Busy; },
            [&](bool ok,int64_t,int64_t) { promise.set_value(ok); },Observer::now()),"Observation accepted");
        check(!observer.submit(active,[]{return Observer::State::Ready;},[](bool,int64_t,int64_t){},
                              Observer::now()),"Capacity is bounded without waiting for GPU");
        ready->store(true);
        check(future.wait_for(std::chrono::seconds(1))==std::future_status::ready && future.get(),
              "Pending surface becomes ready without producer wait");

        std::promise<bool> expired;auto expiry=expired.get_future();
        Observer short_observer(1,std::chrono::milliseconds(5));
        check(short_observer.submit(active,[]{return Observer::State::Busy;},
              [&](bool ok,int64_t,int64_t){expired.set_value(ok);},Observer::now()),"Timeout job accepted");
        check(expiry.wait_for(std::chrono::seconds(1))==std::future_status::ready && !expiry.get(),
              "Timeout is missing data, never a zero-latency success");

        auto cancelled=std::make_shared<std::atomic<bool>>(false);
        std::atomic<unsigned> queries{0},callbacks{0};
        { Observer cancel_observer;
          check(cancel_observer.submit(cancelled,[&]{++queries;return Observer::State::Ready;},
                [&](bool,int64_t,int64_t){++callbacks;},Observer::now()),"Cancelled job accepted for cleanup"); }
        check(!queries && !callbacks,"Teardown cancels queries and completion callbacks");
        std::promise<bool> failed;auto failure=failed.get_future();
        Observer errors;
        check(errors.submit(active,[]()->Observer::State {throw std::runtime_error("query failed");},
              [&](bool ok,int64_t,int64_t){failed.set_value(ok);},Observer::now()),"Failure job accepted");
        check(failure.wait_for(std::chrono::seconds(1))==std::future_status::ready && !failure.get(),
              "Query errors cannot be reported as successful readiness");
        std::cout<<"Decode readiness contracts passed\n";
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
