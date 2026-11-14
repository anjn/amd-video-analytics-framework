#pragma once
#include <mutex>
#include <stdexcept>
#include <utility>
namespace amdva::nvr::detail {
// Only the producer owns state 4. Consumers cannot display or reuse that plane.
// Preserve the caller's lock ownership on both success and every exception.
template<class Plane,class Prepare,class StillCurrent>
bool publish_preview_unlocked(Plane &plane,std::unique_lock<std::mutex> &lock,
                              Prepare prepare,StillCurrent current) {
    if(!lock.owns_lock() || plane.state!=0)
        throw std::logic_error("Preview publication requires a locked free plane");
    Plane prepared;prepared.surface=plane.surface;plane.state=4;
    lock.unlock();
    try { prepare(prepared); }
    catch(...) { lock.lock();plane.state=0;throw; }
    lock.lock();
    if(!current()){plane.state=0;return false;}
    plane=std::move(prepared);plane.state=1;return true;
}
}
