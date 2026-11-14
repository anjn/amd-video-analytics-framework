#pragma once
#include "amdva/diagnostics.hpp"
#include "amdva/telemetry.hpp"
#include <mutex>
namespace amdva::nvr {
// Preserve the existing lock scope; observe it without GPU synchronization.
class DiagnosticSlotLock {
    std::unique_lock<std::mutex> lock_;
    const char *event_;
    unsigned slot_;
    int64_t started_{}, acquired_{};
  public:
    DiagnosticSlotLock(std::mutex &mutex, unsigned slot, const char *event)
        : lock_(mutex, std::defer_lock), event_(event), slot_(slot) {
        if (diagnostics_enabled()) started_=Telemetry::monotonic_ns();
        lock_.lock();
        if (started_) acquired_=Telemetry::monotonic_ns();
    }
    ~DiagnosticSlotLock() {
        if(started_) diagnostic_event("ui-slot",0,slot_+1,event_,Telemetry::monotonic_ns(),
                                      acquired_-started_,Telemetry::monotonic_ns()-acquired_);
    }
};
} // namespace amdva::nvr
