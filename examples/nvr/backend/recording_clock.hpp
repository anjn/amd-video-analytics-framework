#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
namespace amdva::nvr {
// Published by the recording owner only after its first successful push.
// The receive/decode path must never dereference a recorder or query SQLite.
class RecordingClock {
    struct Mapping { uint64_t pts; int64_t wall_ms; std::string session; };
    mutable std::mutex mutex_;
    std::optional<Mapping> mapping_;
  public:
    struct Stamp { int64_t wall_ms; std::string session; };
    void publish(uint64_t pts,int64_t wall_ms,const std::string& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mapping_ || mapping_->session!=session) mapping_=Mapping{pts,wall_ms,session};
    }
    std::optional<Stamp> at(uint64_t pts) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mapping_ || pts<mapping_->pts) return std::nullopt;
        return Stamp{mapping_->wall_ms+int64_t((pts-mapping_->pts)/1000000),mapping_->session};
    }
    void clear() { std::lock_guard<std::mutex> lock(mutex_); mapping_.reset(); }
};
}
