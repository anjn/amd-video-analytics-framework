#pragma once
#include "amdva/resource_monitor.hpp"
#include <cmath>

namespace amdva::dashboard {
// Presentation only: never change the monitor's raw values or the helper cadence.
class GpuDisplayFilter {
  public:
    explicit GpuDisplayFilter(bool wave_enabled) : wave_enabled_(wave_enabled) {}

    ResourceSample apply(ResourceSample sample) {
        if (!wave_enabled_)
            return sample;
        const auto now = sample.monotonic_ns;
        const bool recent = value_ && now >= last_valid_ && now - last_valid_ <= hold_ns;
        if (sample.gpu_source == "wave" && sample.gpu_pct &&
            std::isfinite(*sample.gpu_pct) && *sample.gpu_pct >= 0 && *sample.gpu_pct <= 100) {
            if (recent && now >= last_update_) {
                const double alpha = -std::expm1(-double(now - last_update_) / smoothing_ns);
                *value_ += alpha * (*sample.gpu_pct - *value_);
            } else
                value_ = sample.gpu_pct;
            last_valid_ = now;
            sample.gpu_pct = value_;
            sample.gpu_status = "ok (3s EMA)";
        } else if (recent) {
            sample.gpu_pct = value_;
            sample.gpu_source = "wave-held";
            sample.gpu_status = "holding wave sample: " + sample.gpu_status;
        } else {
            value_.reset();
            sample.gpu_pct.reset();
            sample.gpu_source = "unavailable";
            sample.gpu_status = "wave unavailable: " + sample.gpu_status;
        }
        last_update_ = now;
        return sample;
    }

  private:
    static constexpr double smoothing_ns = 3000000000.;
    static constexpr int64_t hold_ns = 2000000000LL;
    bool wave_enabled_;
    std::optional<double> value_;
    int64_t last_valid_{}, last_update_{};
};
} // namespace amdva::dashboard
