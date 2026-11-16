#include "app/presentation_latency.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using amdva::nvr::detail::PresentationLatency;
constexpr int64_t ms=1000000, second=1000000000;
void check(bool value,const char *why){if(!value)throw std::runtime_error(why);}
void close(double value,double expected){check(std::abs(value-expected)<1e-8,"Incorrect latency value");}
int main(){try{
    PresentationLatency metric;
    check(!metric.snapshot(1,second).available,"No samples must be N/A");
    check(metric.observe(1,1,10*ms,160*ms,190*ms),"Accept presentation");
    auto first=metric.snapshot(1,190*ms);
    check(first.available&&first.samples==1,"First complete sample");
    close(first.mean_ms,150); // Not 180ms: callback receipt is not presentation.
    check(metric.observe(1,2,30*ms,230*ms,300*ms),"Second presentation");
    auto both=metric.snapshot(1,300*ms);close(both.mean_ms,175);close(both.maximum_ms,200);
    check(!metric.observe(1,2,30*ms,230*ms,310*ms),"Duplicate feedback ignored");
    check(metric.snapshot(1,310*ms).samples==2,"One sample per picture");
    check(!metric.snapshot(2,310*ms).available,"Old generation cannot label a new camera");
    auto tail=metric.snapshot(1,10*second+190*ms+1);
    check(tail.samples==1&&tail.available,"Expire the first sample");close(tail.mean_ms,200);
    check(!metric.snapshot(1,10*second+300*ms+1).available,"Stopped input expires to N/A");
    check(metric.observe(2,1,20*second,20*second+50*ms,20*second+60*ms),"New generation");
    close(metric.snapshot(2,20*second+60*ms).mean_ms,50);
    check(!metric.observe(1,99,20*second,20*second+70*ms,20*second+80*ms),"Old callback cannot reset new stats");
    close(metric.snapshot(2,20*second+80*ms).maximum_ms,50);
    PresentationLatency missing;
    check(!missing.observe(1,1,0,150*ms,160*ms),"Missing input is not zero latency");
    missing.observe(1,2,100*ms,170*ms,180*ms);
    check(!missing.snapshot(1,180*ms).available,"Incomplete window must be N/A, not a biased mean");
    PresentationLatency future;
    check(future.observe(1,1,100*ms,170*ms,160*ms),"Reported presentation may lead callback receipt");
    close(future.snapshot(1,161*ms).mean_ms,70);
    PresentationLatency negative;
    check(!negative.observe(1,1,180*ms,170*ms,190*ms),"Negative duration rejected");
    PresentationLatency peak;
    peak.observe(1,1,1*ms,150*ms,151*ms);
    peak.observe(1,2,100*ms,160*ms,161*ms);
    close(peak.snapshot(1,161*ms).maximum_ms,149);
    close(peak.snapshot(1,10*second+151*ms+1).maximum_ms,60);
    PresentationLatency bounded(10*second,2);
    bounded.observe(1,1,1*ms,100*ms,101*ms);
    bounded.observe(1,2,2*ms,110*ms,111*ms);
    bounded.observe(1,3,3*ms,120*ms,121*ms);
    check(!bounded.snapshot(1,121*ms).available,"Capacity truncation cannot pretend to cover the window");
    check(bounded.snapshot(1,10*second+101*ms+1).available,"Completeness recovers after overflow leaves the window");
    PresentationLatency boundary;
    boundary.observe(1,1,1*ms,100*ms,101*ms);
    check(boundary.snapshot(1,10*second+101*ms).available,"Exact window boundary included");
    check(!boundary.snapshot(1,10*second+101*ms+1).available,"Beyond boundary excluded");
    PresentationLatency wrapped;
    for(uint64_t i=1;i<=1200;++i) {
        const int64_t at=second+int64_t(i)*33333334;
        check(wrapped.observe(1,i,at-int64_t(50+(i%5)*10)*ms,at,at+ms),"Long-window admission");
    }
    auto steady=wrapped.snapshot(1,second+1200LL*33333334+ms);
    check(steady.available&&steady.samples==300,"Ring wrap preserves the complete ten-second window");
    close(steady.mean_ms,70);close(steady.maximum_ms,90);
    std::cout<<"Presentation latency clock, window, identity and availability contracts passed\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
