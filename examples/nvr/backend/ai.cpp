#include "backend/ai.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace amdva::nvr {
void AiRuntime::configure(double fps, unsigned interval) {
    if ((!std::isfinite(fps) || fps < .1 || fps > 10) || interval < 100 || interval > 60000)
        throw std::invalid_argument("Invalid AI frequency");
    std::lock_guard<std::mutex> lock(mutex_);
    fps_ = fps;
    interval_ = interval;
}
double AiRuntime::fps() const {
    std::lock_guard<std::mutex> l(mutex_);
    return fps_;
}
unsigned AiRuntime::interval_ms() const {
    std::lock_guard<std::mutex> l(mutex_);
    return interval_;
}
void AiRuntime::record(const std::string &stage, const std::string &camera, double ms, double wait,
                       unsigned batch) {
    if (!std::isfinite(ms) || ms < 0 || !std::isfinite(wait) || wait < 0)
        return;
    if (!camera.empty())
        record(stage, "", ms, wait, batch);
    std::lock_guard<std::mutex> l(mutex_);
    auto key = std::make_pair(stage, camera);
    if (!rows_.count(key) && rows_.size() >= 512)
        return;
    auto &r = rows_[key];
    auto now = Clock::now();
    while (!r.samples.empty() && now - r.samples.front().at > std::chrono::seconds(60))
        r.samples.pop_front();
    if (r.samples.size() >= 8192) {
        r.samples.pop_front();
        r.truncated = true;
    }
    r.samples.push_back({now, ms, wait, batch});
    ++r.completed;
}
void AiRuntime::queue(const std::string &s, const std::string &c, unsigned pending,
                      uint64_t drops) {
    std::lock_guard<std::mutex> l(mutex_);
    auto key = std::make_pair(s, c);
    if (!rows_.count(key) && rows_.size() >= 512)
        return;
    auto &r = rows_[key];
    r.pending = pending;
    r.dropped += drops;
}
void AiRuntime::reset_measurements() {
    std::lock_guard<std::mutex> lock(mutex_);
    rows_.clear();
    started_ = Clock::now();
}
std::vector<AiMetric> AiRuntime::snapshot() const {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<AiMetric> out;
    auto now = Clock::now();
    double window =
        std::max(.001, std::min(60., std::chrono::duration<double>(now - started_).count()));
    for (const auto &[key, r] : rows_) {
        AiMetric m;
        m.stage = key.first;
        m.completed = r.completed;
        m.camera = key.second;
        m.pending = r.pending;
        m.dropped = r.dropped;
        m.truncated = r.truncated;
        std::vector<double> v;
        for (const auto &s : r.samples)
            if (now - s.at <= std::chrono::seconds(60)) {
                v.push_back(s.ms);
                m.mean_ms += s.ms;
                m.wait_ms += s.wait;
                m.mean_batch += s.batch;
            }
        m.samples = v.size();
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            m.mean_ms /= v.size();
            m.wait_ms /= v.size();
            m.mean_batch /= v.size();
            m.p50_ms = v[v.size() / 2];
            m.p95_ms = v[std::min(v.size() - 1, size_t(v.size() * .95))];
            m.max_ms = v.back();
            m.rate = v.size() / window;
        }
        out.push_back(m);
    }
    return out;
}
} // namespace amdva::nvr
