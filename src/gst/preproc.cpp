#include "amdva/npu_meta.hpp"
#include "amdva/tensor_meta.hpp"
#include "amdva/video_preprocess.hpp"
#include <algorithm>
#include <cmath>
#include <gst/base/gstbasetransform.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>
#include <sstream>

GST_DEBUG_CATEGORY_STATIC(preproc_debug);
#define GST_CAT_DEFAULT preproc_debug
struct PreprocState {
    GstVideoInfo video{};
    std::unique_ptr<amdva::VideoPreprocessor> processor;
    amdva::ModelInfo info;
};
typedef struct {
    GstBaseTransform parent;
    gint device;
    guint width, height;
    gboolean strict, letterbox, roi_meta;
    gdouble scale;
    gchar *mean, *stddev;
    gchar *model_info;
    gchar *output_memory;
    PreprocState *state;
} AmdPreproc;
typedef struct {
    GstBaseTransformClass parent;
} AmdPreprocClass;
G_DEFINE_TYPE(AmdPreproc, amd_preproc, GST_TYPE_BASE_TRANSFORM)
enum {
    P_NONE,
    P_DEVICE,
    P_WIDTH,
    P_HEIGHT,
    P_STRICT,
    P_INFO,
    P_MEMORY,
    P_LETTERBOX,
    P_SCALE,
    P_MEAN,
    P_STD,
    P_ROI
};
static GstStaticPadTemplate sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static GstStaticPadTemplate src = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    if (GST_STATE(self) > GST_STATE_READY) {
        GST_WARNING_OBJECT(self, "Properties can only change in NULL or READY");
        return;
    }
    switch (p) {
    case P_DEVICE:
        self->device = g_value_get_int(v);
        break;
    case P_WIDTH:
        self->width = g_value_get_uint(v);
        break;
    case P_HEIGHT:
        self->height = g_value_get_uint(v);
        break;
    case P_STRICT:
        self->strict = g_value_get_boolean(v);
        break;
    case P_INFO:
        g_free(self->model_info);
        self->model_info = g_value_dup_string(v);
        break;
    case P_LETTERBOX:
        self->letterbox = g_value_get_boolean(v);
        break;
    case P_SCALE:
        self->scale = g_value_get_double(v);
        break;
    case P_ROI:
        self->roi_meta = g_value_get_boolean(v);
        break;
    case P_MEAN:
        g_free(self->mean);
        self->mean = g_value_dup_string(v);
        break;
    case P_STD:
        g_free(self->stddev);
        self->stddev = g_value_dup_string(v);
        break;
    case P_MEMORY:
        g_free(self->output_memory);
        self->output_memory = g_value_dup_string(v);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    switch (p) {
    case P_DEVICE:
        g_value_set_int(v, self->device);
        break;
    case P_WIDTH:
        g_value_set_uint(v, self->width);
        break;
    case P_HEIGHT:
        g_value_set_uint(v, self->height);
        break;
    case P_STRICT:
        g_value_set_boolean(v, self->strict);
        break;
    case P_INFO:
        g_value_set_string(v, self->model_info);
        break;
    case P_LETTERBOX:
        g_value_set_boolean(v, self->letterbox);
        break;
    case P_SCALE:
        g_value_set_double(v, self->scale);
        break;
    case P_ROI:
        g_value_set_boolean(v, self->roi_meta);
        break;
    case P_MEAN:
        g_value_set_string(v, self->mean);
        break;
    case P_STD:
        g_value_set_string(v, self->stddev);
        break;
    case P_MEMORY:
        g_value_set_string(v, self->output_memory);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static std::array<float, 3> triple(const char *text) {
    std::string value = text ? text : "";
    std::replace(value.begin(), value.end(), ',', ' ');
    std::istringstream in(value);
    std::array<float, 3> result;
    for (auto &v : result)
        if (!(in >> v) || !std::isfinite(v))
            throw std::invalid_argument("mean/std require three finite values");
    in >> std::ws;
    if (!in.eof())
        throw std::invalid_argument("mean/std require exactly three values");
    return result;
}
static gboolean start(GstBaseTransform *t) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    if (!self->strict) {
        GST_ELEMENT_ERROR(self, LIBRARY, SETTINGS, ("Only strict zero-copy mode is implemented"),
                          (nullptr));
        return FALSE;
    }
    try {
        if (!self->output_memory || (std::string(self->output_memory) != "device" &&
                                     std::string(self->output_memory) != "host-mapped"))
            throw std::invalid_argument("output-memory must be device or host-mapped");
        auto state = std::make_unique<PreprocState>();
        amdva::ModelInfo info;
        if (self->model_info)
            info = amdva::ModelInfo::load(self->model_info);
        else {
            info.width = self->width;
            info.height = self->height;
            info.letterbox = self->letterbox;
            info.normalization.scale = float(self->scale);
            info.normalization.mean = triple(self->mean);
            info.normalization.stddev = triple(self->stddev);
            if (!std::isfinite(info.normalization.scale) ||
                std::any_of(info.normalization.stddev.begin(), info.normalization.stddev.end(),
                            [](float v) { return v <= 0; }))
                throw std::invalid_argument("Invalid normalization scale/std");
        }
        state->processor = std::make_unique<amdva::VideoPreprocessor>(info, self->device);
        state->info = info;
        delete self->state;
        self->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, LIBRARY, INIT, ("Cannot initialize preprocessing"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    delete self->state;
    self->state = nullptr;
    return TRUE;
}
static gboolean set_caps(GstBaseTransform *t, GstCaps *in, GstCaps *) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    if (!self->state)
        return FALSE;
    auto &video = self->state->video;
    if (!gst_video_info_from_caps(&video, in) ||
        GST_VIDEO_INFO_FORMAT(&video) != GST_VIDEO_FORMAT_NV12 || video.width % 2 ||
        video.height % 2 || video.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE)
        return FALSE;
    // VPE CSC is explicitly BT.709 limited-range. Reject other declared matrices/ranges.
    if ((video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
         video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
        (video.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
         video.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
        return FALSE;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *buffer) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    try {
        if (amdva::get_tensor_meta(buffer) || amdva::get_shared_tensor_meta(buffer))
            throw std::runtime_error("Input already carries an AMD tensor");
        unsigned x = 0, y = 0, w = 0, h = 0;
        if (self->roi_meta) {
            gpointer iter = nullptr;
            GstVideoRegionOfInterestMeta *roi = nullptr;
            while (auto *meta = gst_buffer_iterate_meta_filtered(
                       buffer, &iter, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
                if (roi)
                    throw std::invalid_argument("roi-meta requires exactly one ROI");
                roi = reinterpret_cast<GstVideoRegionOfInterestMeta *>(meta);
            }
            if (!roi || !roi->w || !roi->h || roi->x >= unsigned(self->state->video.width) ||
                roi->y >= unsigned(self->state->video.height) ||
                roi->w > unsigned(self->state->video.width) - roi->x ||
                roi->h > unsigned(self->state->video.height) - roi->y)
                throw std::invalid_argument("roi-meta requires one nonempty ROI inside the video");
            x = roi->x;
            y = roi->y;
            w = roi->w;
            h = roi->h;
        }
        if (std::string(self->output_memory) == "host-mapped") {
            auto &info = self->state->info;
            auto shared = std::make_shared<amdva::SharedTensor>(
                amdva::TensorDescriptor{
                    info.input_name, amdva::TensorType::Float32, {1, 3, info.height, info.width}},
                self->device);
            self->state->processor->prepare_into(buffer, self->state->video, shared, x, y, w, h);
            if (!amdva::add_shared_tensor_meta(buffer, std::move(shared)))
                throw std::runtime_error("Cannot attach shared tensor");
            return GST_FLOW_OK;
        }
        auto tensor = self->state->processor->prepare(buffer, self->state->video, x, y, w, h);
        if (!amdva::add_tensor_meta(buffer, std::move(tensor)))
            throw std::runtime_error("Cannot attach tensor metadata");
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("AMD preprocessing failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *o) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    delete self->state;
    g_free(self->model_info);
    g_free(self->output_memory);
    g_free(self->mean);
    g_free(self->stddev);
    G_OBJECT_CLASS(amd_preproc_parent_class)->finalize(o);
}
static void amd_preproc_class_init(AmdPreprocClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    const auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_LETTERBOX,
        g_param_spec_boolean("letterbox", "Letterbox",
                             "Preserve aspect ratio with padding (model-info takes precedence)",
                             TRUE, flags));
    g_object_class_install_property(
        object, P_ROI,
        g_param_spec_boolean("roi-meta", "ROI metadata",
                             "Preprocess exactly one GstVideoRegionOfInterestMeta rectangle", FALSE,
                             flags));
    g_object_class_install_property(
        object, P_SCALE,
        g_param_spec_double("tensor-scale", "Tensor scale",
                            "Pixel scale before mean/std (model-info takes precedence)",
                            -G_MAXDOUBLE, G_MAXDOUBLE, 1.0 / 255, flags));
    g_object_class_install_property(
        object, P_MEAN,
        g_param_spec_string("tensor-mean", "Tensor mean",
                            "Three RGB means, comma separated (model-info takes precedence)",
                            "0,0,0", flags));
    g_object_class_install_property(
        object, P_STD,
        g_param_spec_string("tensor-std", "Tensor standard deviation",
                            "Three positive RGB standard deviations (model-info takes precedence)",
                            "1,1,1", flags));
    g_object_class_install_property(object, P_MEMORY,
                                    g_param_spec_string("output-memory", "Output memory",
                                                        "device or host-mapped (NPU shared input)",
                                                        "device", flags));
    g_object_class_install_property(
        object, P_DEVICE,
        g_param_spec_int("device", "GPU device", "HIP device index", 0, 16, 0, flags));
    g_object_class_install_property(object, P_WIDTH,
                                    g_param_spec_uint("tensor-width", "Tensor width",
                                                      "Model input width", 2, 4096, 640, flags));
    g_object_class_install_property(object, P_HEIGHT,
                                    g_param_spec_uint("tensor-height", "Tensor height",
                                                      "Model input height", 2, 4096, 640, flags));
    g_object_class_install_property(object, P_STRICT,
                                    g_param_spec_boolean("require-zero-copy", "Strict mode",
                                                         "Reject copies and non-VPE scaling", TRUE,
                                                         flags));
    g_object_class_install_property(object, P_INFO,
                                    g_param_spec_string("model-info", "Model info",
                                                        "Model preprocessing JSON", nullptr,
                                                        flags));
    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "AMD VPE/HIP preprocessing", "Filter/Video",
                                          "Preserves VA video and attaches GPU tensor", "AMD");
    gst_element_class_add_static_pad_template(element, &sink);
    gst_element_class_add_static_pad_template(element, &src);
    auto *base = GST_BASE_TRANSFORM_CLASS(klass);
    base->start = start;
    base->stop = stop;
    base->set_caps = set_caps;
    base->transform_ip = transform;
    GST_DEBUG_CATEGORY_INIT(preproc_debug, "amdpreproc", 0, "AMD VPE/HIP preprocessing");
}
static void amd_preproc_init(AmdPreproc *self) {
    self->output_memory = g_strdup("device");
    self->mean = g_strdup("0,0,0");
    self->stddev = g_strdup("1,1,1");
    self->scale = 1.0 / 255;
    self->letterbox = TRUE;
    self->roi_meta = FALSE;
    self->device = 0;
    self->width = 640;
    self->height = 640;
    self->strict = TRUE;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
