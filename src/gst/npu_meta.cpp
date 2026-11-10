#include "amdva/npu_meta.hpp"
namespace amdva {
namespace {
gboolean init_input(GstMeta *m, gpointer, GstBuffer *) {
    reinterpret_cast<SharedTensorMeta *>(m)->tensor = nullptr;
    return TRUE;
}
gboolean init_output(GstMeta *m, gpointer, GstBuffer *) {
    reinterpret_cast<NpuResultMeta *>(m)->result = nullptr;
    return TRUE;
}
void free_input(GstMeta *m, GstBuffer *) { delete reinterpret_cast<SharedTensorMeta *>(m)->tensor; }
void free_output(GstMeta *m, GstBuffer *) { delete reinterpret_cast<NpuResultMeta *>(m)->result; }
gboolean copy_input(GstBuffer *out, GstMeta *m, GstBuffer *, GQuark type, gpointer data) {
    if (!GST_META_TRANSFORM_IS_COPY(type) || static_cast<GstMetaTransformCopy *>(data)->region)
        return FALSE;
    auto *s = reinterpret_cast<SharedTensorMeta *>(m);
    return s->tensor && add_shared_tensor_meta(out, *s->tensor);
}
gboolean copy_output(GstBuffer *out, GstMeta *m, GstBuffer *, GQuark type, gpointer data) {
    if (!GST_META_TRANSFORM_IS_COPY(type) || static_cast<GstMetaTransformCopy *>(data)->region)
        return FALSE;
    auto *s = reinterpret_cast<NpuResultMeta *>(m);
    return s->result && add_npu_result_meta(out, *s->result);
}
const GstMetaInfo *input_info() {
    static gsize info = 0;
    if (g_once_init_enter(&info)) {
        auto *m = gst_meta_register(shared_tensor_meta_api(), "AmdSharedTensorMeta",
                                    sizeof(SharedTensorMeta), init_input, free_input, copy_input);
        g_once_init_leave(&info, reinterpret_cast<gsize>(m));
    }
    return reinterpret_cast<const GstMetaInfo *>(info);
}
const GstMetaInfo *output_info() {
    static gsize info = 0;
    if (g_once_init_enter(&info)) {
        auto *m = gst_meta_register(npu_result_meta_api(), "AmdNpuResultMeta",
                                    sizeof(NpuResultMeta), init_output, free_output, copy_output);
        g_once_init_leave(&info, reinterpret_cast<gsize>(m));
    }
    return reinterpret_cast<const GstMetaInfo *>(info);
}
} // namespace
GType shared_tensor_meta_api() {
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        const char *tags[] = {"amd-shared-tensor", nullptr};
        auto t = gst_meta_api_type_register("AmdSharedTensorMetaAPI", tags);
        g_once_init_leave(&type, t);
    }
    return GType(type);
}
GType npu_result_meta_api() {
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        const char *tags[] = {"amd-npu-result", nullptr};
        auto t = gst_meta_api_type_register("AmdNpuResultMetaAPI", tags);
        g_once_init_leave(&type, t);
    }
    return GType(type);
}
SharedTensorMeta *add_shared_tensor_meta(GstBuffer *b, std::shared_ptr<SharedTensor> tensor) {
    if (!tensor || get_shared_tensor_meta(b))
        return nullptr;
    auto *m = reinterpret_cast<SharedTensorMeta *>(gst_buffer_add_meta(b, input_info(), nullptr));
    if (!m)
        return nullptr;
    try {
        m->tensor = new std::shared_ptr<SharedTensor>(std::move(tensor));
    } catch (...) {
        gst_buffer_remove_meta(b, &m->meta);
        return nullptr;
    }
    return m;
}
SharedTensorMeta *get_shared_tensor_meta(GstBuffer *b) {
    return reinterpret_cast<SharedTensorMeta *>(gst_buffer_get_meta(b, shared_tensor_meta_api()));
}
NpuResultMeta *add_npu_result_meta(GstBuffer *b, std::shared_ptr<NpuResult> result) {
    if (!result || get_npu_result_meta(b))
        return nullptr;
    auto *m = reinterpret_cast<NpuResultMeta *>(gst_buffer_add_meta(b, output_info(), nullptr));
    if (!m)
        return nullptr;
    try {
        m->result = new std::shared_ptr<NpuResult>(std::move(result));
    } catch (...) {
        gst_buffer_remove_meta(b, &m->meta);
        return nullptr;
    }
    return m;
}
NpuResultMeta *get_npu_result_meta(GstBuffer *b) {
    return reinterpret_cast<NpuResultMeta *>(gst_buffer_get_meta(b, npu_result_meta_api()));
}
} // namespace amdva
