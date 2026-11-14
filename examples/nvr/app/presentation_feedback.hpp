#pragma once
#include "amdva/gst_telemetry.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
class QWindow;
namespace amdva::nvr {
struct FeedbackFrame {
    unsigned slot{}, plane{};
    uint64_t generation{}, sequence{};
    TraceToken timing;
};
// UI-thread only. Borrows Qt's display/surface, owns a separate Wayland event
// queue and bounded feedback metadata. No image access, GPU command or GPU wait.
class PresentationFeedback {
  public:
    using Callback=std::function<void(const std::vector<FeedbackFrame>&,int64_t,uint32_t,bool)>;
    PresentationFeedback(QWindow &,Callback,std::function<void(std::string)> failure);
    ~PresentationFeedback();
    void submit(std::vector<FeedbackFrame>);
    void poll();
    int64_t refresh_ns() const;
    bool pending(const FeedbackFrame &) const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
