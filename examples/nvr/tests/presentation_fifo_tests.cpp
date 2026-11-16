#include "backend/presentation_fifo.hpp"
#include <future>
#include <thread>
#include <iostream>
#include <limits>
using namespace amdva::nvr;
using namespace std::chrono_literals;
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,"Expected explicit failure");}
using Queue=PresentationFifo<int>;
int main(){
 try {
  int64_t now=1000000000;Queue q(3,2,100ms,[&]{return now;});q.reset(7,true);
  for(uint64_t i=0;i<3;i++){check(q.push(int(i),7,i+1,i*33333333,33333333)==Queue::Result::Queued,"Admission");now+=33333333;}
  check(q.peek()->serial==1 && q.peek()->due_ns==1066666666,"PTS-anchored prefill");
  check(q.snapshot().frames==3,"Bounded depth");
  std::promise<void> entered;auto producer=std::async(std::launch::async,[&]{
   entered.set_value();return q.push(3,7,4,99999999,33333333);
  });entered.get_future().get();
  check(producer.wait_for(20ms)==std::future_status::timeout,"Full must backpressure");
  check(q.pop(7,1)->frame==0,"Oldest frame preserved");
  check(producer.get()==Queue::Result::Queued,"Resume producer");
  for(uint64_t i=2;i<=4;i++)check(q.pop(7,i)->frame==int(i-1),"FIFO order, no overwrite");
  check(!q.peek(),"Empty");
  check(!q.pop(8,4),"Generation cannot consume another queue");
  q.reset(8,true);q.push(1,8,1,0,100000000);
  check(q.peek()->due_ns==now+200000000,"10fps resource cadence");
  rejects([&]{q.push(2,8,2,0,100000000);});
  check(!q.snapshot().error.empty(),"Failure remains latched");
  rejects([&]{q.push(3,8,3,100000000,100000000);});
  auto retired=q.reset(9,true);check(retired.size()==1,"Reset returns every retired identity");
  check(q.push(4,8,2,100000000,100000000)==Queue::Result::Retired,"Stale producer explicit");
  for(uint64_t i=0;i<3;i++)q.push(int(i),9,i+1,i*33333333,33333333);
  std::promise<void> waiting;auto closing=std::async(std::launch::async,[&]{
   waiting.set_value();return q.push(4,9,4,99999999,33333333);
  });waiting.get_future().get();check(closing.wait_for(20ms)==std::future_status::timeout,"Close fixture blocked");
  check(q.close().size()==3,"Close returns retained frames");
  check(closing.get()==Queue::Result::Retired,"Close wakes producer without losing identity");
  q.reset(10,true);q.push(1,10,1,0,33333333);
  check(q.active(false).size()==1,"Visibility suspension is explicit");
  check(q.push(2,10,2,33333333,33333333)==Queue::Result::Inactive,"Inactive does not silently admit");
  q.active(true);now+=1000000000;q.push(3,10,3,66666666,33333333);
  check(q.peek()->due_ns==now+66666666,"Resume reanchors presentation, no stale deadline");
  Queue full(1,0,10ms);full.reset(1,true);full.push(1,1,1,0,33333333);
  rejects([&]{full.push(2,1,2,33333333,33333333);});
  check(full.peek()->frame==1,"Timeout does not overwrite");
  Queue bad;bad.reset(1,true);
  rejects([&]{bad.push(1,1,0,0,33333333);});
  rejects([&]{Queue zero(0,0);});
  rejects([&]{Queue impossible(3,3);});
  Queue reversed;reversed.reset(1,true);reversed.push(1,1,1,100,33333333);
  rejects([&]{reversed.push(2,1,2,99,33333333);});
  Queue duplicate;duplicate.reset(1,true);duplicate.push(1,1,2,0,33333333);
  rejects([&]{duplicate.push(2,1,2,33333333,33333333);});
  Queue rate;rate.reset(1,true);rate.push(1,1,1,0,33333333);
  rejects([&]{rate.push(2,1,2,33333333,33333334);});
  Queue overflow(3,2,100ms,[]{return std::numeric_limits<int64_t>::max()-1;});
  overflow.reset(1,true);rejects([&]{overflow.push(1,1,1,0,33333333);});
  int64_t delayed=1000000000;Queue late(3,2,100ms,[&]{return delayed;});
  late.reset(1,true);late.push(1,1,1,0,33333333);late.pop(1,1);
  delayed+=300000000;rejects([&]{late.push(2,1,2,33333333,33333333);});
  check(!late.peek(),"Late input is rejected explicitly, never overwrites");
  Queue epoch(1,0,500ms);epoch.reset(1,true);epoch.push(1,1,1,0,33333333);
  auto suspended=std::async(std::launch::async,[&]{return epoch.push(2,1,2,33333333,33333333);});
  const auto blocked_until=std::chrono::steady_clock::now()+1s;
  while(!epoch.snapshot().full_waits && std::chrono::steady_clock::now()<blocked_until)
      std::this_thread::sleep_for(1ms);
  check(epoch.snapshot().full_waits==1,"Suspension fixture entered its full wait");
  check(epoch.active(false).size()==1,"Suspend returns existing identity");epoch.active(true);
  check(suspended.get()==Queue::Result::Retired,"Old epoch cannot enter after resume");
  check(!epoch.peek(),"Resumed queue contains no stale blocked input");
  int64_t burst_now=1000000000;
  Queue burst(3,2,100ms,[&]{return burst_now;},Queue::Policy::DropOldest);
  burst.reset(1,true);
  for(unsigned i=0;i<20;++i) {
    burst.push(i,1,i+1,uint64_t(i)*33333333,33333333);burst_now+=1000000;
  }
  check(burst.snapshot().clock_resyncs>0 &&
        burst.peek()->due_ns<=burst_now+33333333,
        "A startup burst cannot perpetually evict future-dated heads");
  int64_t live_now=1000000000;
  Queue live(3,2,100ms,[&]{return live_now;},Queue::Policy::DropOldest);
  live.reset(1,true);
  for(unsigned i=0;i<3;++i) { live.push(i,1,i+1,i*33333333,33333333);live_now+=33333333; }
  auto peeked=live.peek();std::vector<Queue::Dropped> dropped;
  check(live.push(4,1,4,99999999,33333333,&dropped)==Queue::Result::Queued,
        "Live FIFO never waits for a full consumer");
  check(dropped.size()==1 && dropped[0].entry.serial==1 &&
        dropped[0].reason==Queue::DropReason::Full,"Overflow reports exact oldest identity");
  check(!live.pop(peeked->generation,peeked->serial),"Stale peek cannot consume a replacement");
  check(live.peek()->serial==2 && live.snapshot().frames==3 && live.snapshot().error.empty(),
        "Newest admission is bounded and leaves no latched pressure error");
  auto in_flight=live.pop(1,2);check(in_flight->frame==1,"Consumer retains its own sample");
  live_now+=500000000;dropped.clear();
  live.push(5,1,5,133333332,33333333,&dropped);
  check(dropped.size()==2 && dropped[0].entry.serial==3 && dropped[1].entry.serial==4,
        "Late recovery retires queued frames only, never the consumer-owned frame");
  check(in_flight->frame==1 && live.peek()->serial==5 &&
        live.peek()->due_ns==live_now && live.snapshot().late_recoveries==1,
        "Late recovery catches up without touching in-flight resources");
  check(live.pop(1,5)->serial==5,"Recovered frame can display immediately");
  live_now+=33333333;live.push(6,1,6,166666665,33333333);
  check(live.pop(1,6)->serial==6,
        "Presentation order remains increasing after frame loss");
  rejects([&]{live.push(7,1,7,100,33333333);});
  check(!live.snapshot().error.empty(),"Live policy does not conceal corrupt PTS");
  std::cout<<"Presentation FIFO ordering, pressure, cutoff, cadence and lifecycle passed\n";
  return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
