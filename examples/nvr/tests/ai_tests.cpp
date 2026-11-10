#include "backend/ai.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
using namespace amdva::nvr;
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main() {
    try {
        AiRuntime r;
        r.configure(5, 15000);
        require(r.fps() == 5 && r.interval_ms() == 15000, "Requested interval is a lower bound");
        bool invalid = false;
        try {
            r.configure(0, 5000);
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        require(invalid && r.fps() == 5, "Invalid configuration is atomic");
        r.configure(.3, 700);
        require(r.fps() == .3 && r.interval_ms() == 700, "Fractional Hz and subsecond intervals");
        r.record("inference", "cam0", 2, 3, 4);
        r.record("inference", "cam0", 4, 5, 2);
        r.record("inference", "cam0", std::numeric_limits<double>::quiet_NaN());
        r.queue("inference", "cam0", 8, 2);
        r.queue("inference", "cam0", 7, 1);
        auto m = r.snapshot().back();
        require(r.snapshot().front().camera.empty() && r.snapshot().front().mean_ms == 3,
                "Cross-camera summary");
        require(m.samples == 2 && m.mean_ms == 3 && m.wait_ms == 4 && m.mean_batch == 3,
                "Timing aggregation");
        require(m.pending == 7 && m.dropped == 3 && m.max_ms == 4, "Queue counters");
        for (int i = 0; i < 9000; ++i)
            r.record("inference", "cam0", 1);
        require(r.snapshot().front().samples == 8192 && r.snapshot().front().truncated,
                "Bounded statistics");
        require(r.snapshot().front().completed == 9002, "Cumulative count survives rolling limit");
        r.reset_measurements();
        require(r.snapshot().empty(), "Measurement reset clears counters");
        // Configuration/metrics have no global execution gate or adaptive interval.
        r.configure(4, 3000);
        require(r.fps() == 4 && r.interval_ms() == 3000,
                "Frequency configuration unchanged by load");
        std::cout << "AI statistics and frequency controls passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
