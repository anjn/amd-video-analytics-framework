#pragma once
#include "amdva/shared_tensor.hpp"
namespace amdva {
struct NpuModelConfig {
    std::string model, provider_config, cache_dir, cache_key;
    int gpu_device{};
};
// Vitis AI EP, one static float32 NCHW RGB input, batch one. No CPU EP fallback.
// Run results retain ORT allocations independently of subsequent calls/executor lifetime.
class NpuExecutor {
  public:
    explicit NpuExecutor(const NpuModelConfig &);
    ~NpuExecutor();
    const TensorDescriptor &input() const;
    const std::vector<TensorDescriptor> &outputs() const;
    std::shared_ptr<NpuResult> infer(const std::shared_ptr<SharedTensor> &);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace amdva
