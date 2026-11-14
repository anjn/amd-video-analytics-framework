#include "backend/preview_publication.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
using amdva::nvr::detail::publish_preview_unlocked;
struct Plane {unsigned surface{};int state{};unsigned width{};};
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    std::mutex mutex;Plane p{17,0,0};unsigned generation=1;
    std::promise<void> preparing;auto begun=preparing.get_future();
    std::promise<void> decoded;auto progress=decoded.get_future();std::exception_ptr failure;
    std::thread receiver([&]{try{
        begun.wait();std::lock_guard<std::mutex> lock(mutex);
        require(p.state==4,"In-flight plane must remain reserved");
        ++generation;decoded.set_value();
    }catch(...){failure=std::current_exception();try{decoded.set_value();}catch(...){}}});
    std::unique_lock<std::mutex> lock(mutex);
    const bool published=publish_preview_unlocked(p,lock,[&](Plane &out){
        require(out.surface==17,"Preserve reserved VA surface");preparing.set_value();
        require(progress.wait_for(std::chrono::seconds(1))==std::future_status::ready,
                "Decode progress blocked by preview preparation");out.width=640;
    },[&]{require(lock.owns_lock(),"Generation checked without lock");return generation==1;});
    lock.unlock();receiver.join();if(failure)std::rethrow_exception(failure);lock.lock();
    require(!published&&p.state==0,"Old-generation plane must not become visible");
    require(publish_preview_unlocked(p,lock,[](Plane &out){out.width=800;},[]{return true;}),"Publish current preview");
    require(lock.owns_lock()&&p.state==1&&p.width==800&&p.surface==17,"Atomic publication metadata");
    bool rejected=false;try{publish_preview_unlocked(p,lock,[](Plane&){},[]{return true;});}catch(const std::logic_error&){rejected=true;}
    require(rejected&&p.state==1,"Busy plane was overwritten");
    p.state=0;bool failed=false;
    try{publish_preview_unlocked(p,lock,[](Plane&){throw std::runtime_error("scale failure");},[]{return true;});}
    catch(const std::runtime_error&){failed=true;}
    require(failed&&lock.owns_lock()&&p.state==0&&p.surface==17,"Exception leaked a reserved surface or lock");
    std::cout<<"Preview reservation, concurrent decode, generation and exception cleanup passed\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
