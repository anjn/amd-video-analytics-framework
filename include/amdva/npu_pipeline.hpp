#pragma once
#include "amdva/npu.hpp"
namespace amdva {
// Persistent, serialized appsrc -> amdnpu -> appsink transport. Neither input
// pixels nor raw outputs are copied. Initialize the Ryzen AI environment first.
class NpuPipeline {
  public:
    explicit NpuPipeline(const NpuModelConfig &);
    ~NpuPipeline();
    NpuPipeline(const NpuPipeline &) = delete;
    NpuPipeline &operator=(const NpuPipeline &) = delete;
    std::shared_ptr<NpuResult> infer(const std::shared_ptr<SharedTensor> &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
