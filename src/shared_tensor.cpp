#include "amdva/shared_tensor.hpp"
#include <limits>
namespace amdva {
size_t TensorDescriptor::bytes() const {
    size_t n = 1;
    for (auto d : shape) {
        if (!d || n > std::numeric_limits<size_t>::max() / d)
            throw std::invalid_argument("Invalid tensor dimensions");
        n *= d;
    }
    size_t width;
    switch (type) {
    case TensorType::Float32:
    case TensorType::Int32:
        width = 4;
        break;
    case TensorType::Float16:
        width = 2;
        break;
    case TensorType::Int64:
        width = 8;
        break;
    case TensorType::UInt8:
        width = 1;
        break;
    default:
        throw std::invalid_argument("Unsupported tensor type");
    }
    if (n > std::numeric_limits<size_t>::max() / width)
        throw std::invalid_argument("Tensor size overflow");
    return n * width;
}
SharedTensor::SharedTensor(TensorDescriptor descriptor, int ordinal)
    : descriptor_(std::move(descriptor)), ordinal_(ordinal) {
    const auto size = descriptor_.bytes();
    hip_check(hipSetDevice(ordinal_), "Shared tensor device");
    hip_check(hipHostMalloc(&host_, size, hipHostMallocMapped), "Shared tensor allocation");
    try {
        hip_check(hipHostGetDevicePointer(&gpu_, host_, 0), "Shared tensor GPU pointer");
        hip_check(hipEventCreateWithFlags(&ready_, hipEventDisableTiming), "Shared tensor event");
    } catch (...) {
        (void)hipHostFree(host_);
        throw;
    }
}
SharedTensor::~SharedTensor() {
    (void)hipSetDevice(ordinal_);
    if (recorded_)
        (void)hipEventSynchronize(ready_);
    if (ready_)
        (void)hipEventDestroy(ready_);
    if (host_)
        (void)hipHostFree(host_);
}
void SharedTensor::record_ready(hipStream_t stream) {
    hip_check(hipSetDevice(ordinal_), "Record shared tensor device");
    hip_check(hipEventRecord(ready_, stream), "Record shared tensor readiness");
    recorded_ = true;
}
void SharedTensor::wait_ready() const {
    if (!recorded_)
        throw std::invalid_argument("Shared tensor has no producer completion event");
    hip_check(hipSetDevice(ordinal_), "Wait shared tensor device");
    hip_check(hipEventSynchronize(ready_), "Shared tensor readiness");
}
} // namespace amdva
