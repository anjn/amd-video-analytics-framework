#include "backend/encoded_dispatch.hpp"
#include "backend/recording_clock.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
using amdva::nvr::EncodedDispatch;
struct AU { uint64_t id; bool key; size_t bytes{6}; };
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
EncodedDispatch<AU> make(std::vector<uint64_t>& preroll) {
    return EncodedDispatch<AU>(12,[](const AU&a){return a.bytes;},[](const AU&a){return a.key;},
        [](const AU&a){return a.id;},[&](const AU&a){preroll.push_back(a.id);});
}
int main() { try {
    {
        std::vector<uint64_t> skipped,out;
        auto lane=make(skipped);lane.enable();
        // The recording/control consumer sees the first AU before on_sample.
        lane.activate([&](const AU&a){out.push_back(a.id);},AU{1,true});
        lane.receive({1,true});lane.receive({2,false});
        check(out==std::vector<uint64_t>({1,2}),"bootstrap echo submitted twice");
        lane.pause();lane.receive({3,false});lane.receive({4,true});
        lane.activate([&](const AU&a){out.push_back(a.id);},AU{4,true});
        lane.receive({5,false});
        check(skipped==std::vector<uint64_t>({3}),"preroll classification");
        check(out==std::vector<uint64_t>({1,2,4,5}),"rebind order");
        lane.close();lane.enable();
        lane.receive({1,true});lane.activate([&](const AU&a){out.push_back(a.id);},AU{1,true});
        check(out.back()==1,"new receive generation did not reset identity");
    }
    {
        std::vector<uint64_t> skipped,out;auto lane=make(skipped);lane.enable();
        lane.receive({1,true});lane.receive({2,false});
        const auto full=lane.snapshot();
        check(full.frames==2 && full.bytes==12 && full.oldest_ns>=0,"queue accounting");
        std::promise<void> entered;
        auto producer=std::async(std::launch::async,[&]{entered.set_value();lane.receive({3,false});});
        entered.get_future().wait();const bool blocked=producer.wait_for(30ms)==std::future_status::timeout;
        lane.activate([&](const AU&a){out.push_back(a.id);},AU{1,true});producer.get();
        check(blocked,"full queue did not apply backpressure");
        check(out==std::vector<uint64_t>({1,2,3}),"backpressure lost/overwrote an AU");
        check(lane.snapshot().bytes==0,"queue bytes not drained");
    }
    {
        std::vector<uint64_t> skipped;auto lane=make(skipped);lane.enable();
        lane.receive({1,true});lane.receive({2,false});
        std::promise<void> entered;
        auto producer=std::async(std::launch::async,[&]{entered.set_value();lane.receive({3,false});});
        entered.get_future().wait();const bool blocked=producer.wait_for(30ms)==std::future_status::timeout;
        auto retired=lane.close();producer.get();
        check(blocked && retired.size()==2 && lane.snapshot().frames==0,"stop did not release bounded producer");
    }
    {
        std::vector<uint64_t> skipped;auto lane=make(skipped);lane.enable();
        lane.receive({1,true});lane.receive({2,false});
        bool failed=false;
        try {lane.activate([](const AU&a){if(a.id==2)throw std::runtime_error("sink failure");},AU{1,true});}
        catch(const std::runtime_error&){failed=true;}
        bool latched=false;try{lane.receive({3,false});}catch(const std::runtime_error&){latched=true;}
        check(failed && latched && lane.close().size()==1,"failure silently discarded/replayed work");
    }
    {
        std::vector<uint64_t> skipped,out;auto lane=make(skipped);lane.enable();
        std::promise<void> storage_started,release_storage;
        auto release=release_storage.get_future();
        auto storage=std::async(std::launch::async,[&]{storage_started.set_value();release.wait();});
        storage_started.get_future().wait();
        lane.activate([&](const AU&a){out.push_back(a.id);},AU{1,true});
        auto input=std::async(std::launch::async,[&]{lane.receive({1,true});lane.receive({2,false});lane.receive({3,false});});
        const bool independent=input.wait_for(1s)==std::future_status::ready;
        release_storage.set_value();storage.get();input.get();
        check(independent && out.size()==3,"input waited for storage completion");
    }
    {
        std::vector<uint64_t> skipped;auto lane=make(skipped);lane.enable();
        std::promise<void> entered,release;auto released=release.get_future();
        lane.activate([&](const AU&a){if(a.id==2){entered.set_value();released.wait();}},AU{1,true});
        auto producer=std::async(std::launch::async,[&]{lane.receive({2,false});});
        entered.get_future().wait();
        auto lifecycle=std::async(std::launch::async,[&]{lane.pause();});
        const bool waits=lifecycle.wait_for(30ms)==std::future_status::timeout;
        release.set_value();producer.get();lifecycle.get();
        check(waits && !lane.snapshot().running,"lifecycle raced an in-flight submission");
        lane.close();check(!lane.receive({3,false}),"closed producer not reported as cancelled");
    }
    {
        std::vector<uint64_t> skipped;auto lane=make(skipped);lane.enable();
        bool failed=false;try{lane.activate([](const AU&){},AU{1,true,13});}catch(const std::runtime_error&){failed=true;}
        check(failed && lane.snapshot().frames==0,"oversized bootstrap bypassed the bound");
    }
    {
        amdva::nvr::RecordingClock clock;
        check(!clock.at(100),"inactive recorder produced a stamp");
        clock.publish(123,1000,"a");clock.publish(33333456,1033,"a");
        const auto stamp=clock.at(66666789);
        check(stamp && stamp->wall_ms==1066 && stamp->session=="a","clock anchor changed/rounded twice");
        clock.clear();check(!clock.at(66666789),"stale recording session retained");
        clock.publish(200000000,3000,"b");
        check(!clock.at(199999999) && clock.at(233333333)->wall_ms==3033,"session boundary");
    }
    std::cout<<"Encoded dispatch contracts passed\n";return 0;
} catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;} }
