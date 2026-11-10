#pragma once
#include "amdva/runtime.hpp"
namespace amdva {
enum class TensorType { Float32, Float16, Int32, Int64, UInt8 };
struct TensorDescriptor {
    std::string name;
    TensorType type{TensorType::Float32};
    std::vector<size_t> shape;
    size_t bytes() const;
};
// Dense HIP-mapped host allocation. The producer records readiness after writing.
// Shared ownership covers allocation and event lifetime, including GstMeta copies.
class SharedTensor {
  public:
    SharedTensor(TensorDescriptor, int gpu_device = 0);
    ~SharedTensor();
    SharedTensor(const SharedTensor &) = delete;
    SharedTensor &operator=(const SharedTensor &) = delete;
    const TensorDescriptor &descriptor() const { return descriptor_; }
    int device() const { return ordinal_; }
    void *host_data() const { return host_; }
    void *device_data() const { return gpu_; }
    void record_ready(hipStream_t stream = nullptr);
    void wait_ready() const;

  private:
    TensorDescriptor descriptor_;
    int ordinal_;
    void *host_{}, *gpu_{};
    hipEvent_t ready_{};
    bool recorded_{};
};
struct HostTensor {
    TensorDescriptor descriptor;
    const void *data{};
    std::shared_ptr<void> owner;
};
struct NpuResult {
    std::vector<HostTensor> tensors;
    uintptr_t input_address{};
};
} // namespace amdva
