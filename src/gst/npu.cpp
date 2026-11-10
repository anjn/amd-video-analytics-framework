#include "amdva/npu.hpp"
#include "amdva/npu_meta.hpp"
#include <gst/base/gstbasetransform.h>

typedef struct {
    GstBaseTransform parent;
    gchar *model, *config, *cache_dir, *cache_key;
    gint device;
    amdva::NpuExecutor *executor;
} AmdNpu;
typedef struct {
    GstBaseTransformClass parent;
} AmdNpuClass;
G_DEFINE_TYPE(AmdNpu, amd_npu, GST_TYPE_BASE_TRANSFORM)
enum { P_NONE, P_MODEL, P_CONFIG, P_CACHE_DIR, P_CACHE_KEY, P_DEVICE };
#define NPU_CAPS                                                                                   \
    "application/x-amd-shared-tensor;video/"                                                       \
    "x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"
static GstStaticPadTemplate sink =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(NPU_CAPS));
static GstStaticPadTemplate src =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(NPU_CAPS));
static void set_property(GObject *object, guint property, const GValue *value, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdNpu *>(object);
    if (GST_STATE(s) > GST_STATE_READY) {
        GST_WARNING_OBJECT(s, "Properties require NULL/READY");
        return;
    }
    gchar **target = nullptr;
    switch (property) {
    case P_MODEL:
        target = &s->model;
        break;
    case P_CONFIG:
        target = &s->config;
        break;
    case P_CACHE_DIR:
        target = &s->cache_dir;
        break;
    case P_CACHE_KEY:
        target = &s->cache_key;
        break;
    case P_DEVICE:
        s->device = g_value_get_int(value);
        return;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
        return;
    }
    g_free(*target);
    *target = g_value_dup_string(value);
}
static void get_property(GObject *object, guint property, GValue *value, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdNpu *>(object);
    switch (property) {
    case P_MODEL:
        g_value_set_string(value, s->model);
        break;
    case P_CONFIG:
        g_value_set_string(value, s->config);
        break;
    case P_CACHE_DIR:
        g_value_set_string(value, s->cache_dir);
        break;
    case P_CACHE_KEY:
        g_value_set_string(value, s->cache_key);
        break;
    case P_DEVICE:
        g_value_set_int(value, s->device);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
    }
}
static gboolean start(GstBaseTransform *base) {
    auto *s = reinterpret_cast<AmdNpu *>(base);
    try {
        amdva::NpuModelConfig c{s->model ? s->model : "", s->config ? s->config : "",
                                s->cache_dir ? s->cache_dir : "", s->cache_key ? s->cache_key : "",
                                s->device};
        auto executor = std::make_unique<amdva::NpuExecutor>(c);
        delete s->executor;
        s->executor = executor.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("Cannot initialize NPU inference"), ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *base) {
    auto *s = reinterpret_cast<AmdNpu *>(base);
    delete s->executor;
    s->executor = nullptr;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform *base, GstBuffer *buffer) {
    auto *s = reinterpret_cast<AmdNpu *>(base);
    try {
        auto *meta = amdva::get_shared_tensor_meta(buffer);
        if (!meta || !meta->tensor || !*meta->tensor)
            throw std::runtime_error("Missing shared tensor; use amdpreproc "
                                     "output-memory=host-mapped or attach AmdSharedTensorMeta");
        if (amdva::get_npu_result_meta(buffer))
            throw std::runtime_error("Buffer already carries an NPU result");
        auto result = s->executor->infer(*meta->tensor);
        if (!amdva::add_npu_result_meta(buffer, std::move(result)))
            throw std::runtime_error("Cannot attach NPU result");
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, STREAM, FAILED, ("NPU inference failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *object) {
    auto *s = reinterpret_cast<AmdNpu *>(object);
    delete s->executor;
    g_free(s->model);
    g_free(s->config);
    g_free(s->cache_dir);
    g_free(s->cache_key);
    G_OBJECT_CLASS(amd_npu_parent_class)->finalize(object);
}
static void amd_npu_class_init(AmdNpuClass *klass) {
    auto *o = G_OBJECT_CLASS(klass);
    o->set_property = set_property;
    o->get_property = get_property;
    o->finalize = finalize;
    auto flags = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        o, P_MODEL,
        g_param_spec_string("model", "Model", "Prepared image ONNX model", nullptr, flags));
    g_object_class_install_property(o, P_CONFIG,
                                    g_param_spec_string("provider-config", "Provider configuration",
                                                        "Vitis AI provider JSON configuration",
                                                        nullptr, flags));
    g_object_class_install_property(o, P_CACHE_DIR,
                                    g_param_spec_string("cache-dir", "Cache directory",
                                                        "Prepared Vitis AI cache root", nullptr,
                                                        flags));
    g_object_class_install_property(o, P_CACHE_KEY,
                                    g_param_spec_string("cache-key", "Cache key",
                                                        "Prepared Vitis AI cache key", nullptr,
                                                        flags));
    g_object_class_install_property(o, P_DEVICE,
                                    g_param_spec_int("gpu-device", "Producer GPU",
                                                     "HIP device that writes shared input", 0, 15,
                                                     0, flags));
    auto *e = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(e, "AMD Vitis AI NPU inference", "Filter/Analyzer",
                                          "Static NPU inference from GPU-written shared tensors",
                                          "AMD");
    gst_element_class_add_static_pad_template(e, &sink);
    gst_element_class_add_static_pad_template(e, &src);
    auto *b = GST_BASE_TRANSFORM_CLASS(klass);
    b->start = start;
    b->stop = stop;
    b->transform_ip = transform;
}
static void amd_npu_init(AmdNpu *s) {
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(s), FALSE);
}
static gboolean plugin_init(GstPlugin *p) {
    return gst_element_register(p, "amdnpu", GST_RANK_NONE, amd_npu_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, amdnpu, "AMD Vitis AI NPU inference",
                  plugin_init, "0.1.0", "MIT/X11", PACKAGE,
                  "https://github.com/amd/ryzen-ai-software")
