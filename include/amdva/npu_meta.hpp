#pragma once
#include "amdva/shared_tensor.hpp"
#include <gst/gst.h>
namespace amdva {
struct SharedTensorMeta {
    GstMeta meta;
    std::shared_ptr<SharedTensor> *tensor;
};
struct NpuResultMeta {
    GstMeta meta;
    std::shared_ptr<NpuResult> *result;
};
GType shared_tensor_meta_api();
GType npu_result_meta_api();
SharedTensorMeta *add_shared_tensor_meta(GstBuffer *, std::shared_ptr<SharedTensor>);
SharedTensorMeta *get_shared_tensor_meta(GstBuffer *);
NpuResultMeta *add_npu_result_meta(GstBuffer *, std::shared_ptr<NpuResult>);
NpuResultMeta *get_npu_result_meta(GstBuffer *);
} // namespace amdva
