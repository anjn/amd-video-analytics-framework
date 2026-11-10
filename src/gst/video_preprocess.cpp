#include "amdva/video_preprocess.hpp"
#include <algorithm>
#include <chrono>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <thread>
namespace amdva {
struct VideoPreprocessor::Impl {
    ModelInfo info;
    int device;
    GstVaDisplay *display{};
    std::unique_ptr<VaSession> va;
    ImagePacker pack;
    VASurfaceID surface{VA_INVALID_SURFACE};
    std::unique_ptr<ImportedSurface> image;
    std::array<unsigned, 4> last_geometry{};
    std::vector<std::unique_ptr<VaSession>> scale_contexts;
    std::unique_ptr<VaSession> intermediate_owner;
    std::array<VASurfaceID, 2> intermediate{VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    unsigned intermediate_width{}, intermediate_height{};
    Impl(ModelInfo m, int d) : info(std::move(m)), device(d), pack(d) {}
    void clear_intermediate() {
        if (intermediate_owner)
            for (auto &id : intermediate) {
                intermediate_owner->destroy_surface(id);
                id = VA_INVALID_SURFACE;
            }
        intermediate_owner.reset();
    }
    ~Impl() {
        scale_contexts.clear();
        clear_intermediate();
        image.reset();
        if (va)
            va->destroy_surface(surface);
        va.reset();
        if (display)
            gst_object_unref(display);
    }
};
VideoPreprocessor::VideoPreprocessor(ModelInfo info, int device)
    : impl_(std::make_unique<Impl>(std::move(info), device)) {}
VideoPreprocessor::~VideoPreprocessor() = default;
std::shared_ptr<Tensor> VideoPreprocessor::prepare(GstBuffer *buffer, const GstVideoInfo &video,
                                                   unsigned x, unsigned y, unsigned w, unsigned h) {
    return prepare_into(buffer, video, nullptr, x, y, w, h);
}
std::shared_ptr<Tensor> VideoPreprocessor::prepare_into(GstBuffer *buffer,
                                                        const GstVideoInfo &video,
                                                        std::shared_ptr<SharedTensor> destination,
                                                        unsigned x, unsigned y, unsigned w,
                                                        unsigned h) {
    auto &s = *impl_;
    if (destination &&
        (destination->device() != s.device || destination->descriptor().name != s.info.input_name ||
         destination->descriptor().type != TensorType::Float32 ||
         destination->descriptor().shape != std::vector<size_t>{1, 3, s.info.height, s.info.width}))
        throw std::invalid_argument("Shared preprocessing destination mismatch");
    hip_check(hipSetDevice(s.device), "Preprocessing device");
    if (GST_VIDEO_INFO_FORMAT(&video) != GST_VIDEO_FORMAT_NV12 || video.width % 2 ||
        video.height % 2 || video.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        (video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
         video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
        (video.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
         video.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
        throw std::runtime_error("Requires progressive even-sized BT.709 limited NV12");
    auto *display = gst_va_buffer_peek_display(buffer);
    auto input = gst_va_buffer_get_surface(buffer);
    if (!display || input == VA_INVALID_SURFACE)
        throw std::runtime_error("Missing VA input surface");
    // Expand the ROI to its enclosing 2x2 NV12 chroma rectangle. Preserve the
    // actual rectangle in the inverse transform used by downstream results.
    auto requested = ImageTransform::make(video.width, video.height, s.info.width, s.info.height,
                                          s.info.letterbox, x, y, w, h);
    const unsigned right =
        std::min(unsigned(video.width), (requested.crop_x + requested.crop_width + 1) & ~1u);
    const unsigned bottom =
        std::min(unsigned(video.height), (requested.crop_y + requested.crop_height + 1) & ~1u);
    x = requested.crop_x & ~1u;
    y = requested.crop_y & ~1u;
    const auto geometry =
        ImageTransform::make(video.width, video.height, s.info.width, s.info.height,
                             s.info.letterbox, x, y, right - x, bottom - y);
    const std::array<unsigned, 4> context_geometry{geometry.crop_width, geometry.crop_height,
                                                   geometry.scaled_width, geometry.scaled_height};
    auto va_display = static_cast<VADisplay>(gst_va_display_get_va_dpy(display));
    if (!s.display) {
        s.display = GST_VA_DISPLAY(gst_object_ref(display));
        s.va = std::make_unique<VaSession>(va_display);
        s.surface = s.va->create_bgra(s.info.width, s.info.height);
    }
    if (display != s.display)
        throw std::runtime_error("VA display changed; recreate preprocessor");
    if (s.last_geometry[0] && context_geometry != s.last_geometry) {
        s.va = std::make_unique<VaSession>(va_display);
        s.scale_contexts.clear();
    }
    s.last_geometry = context_geometry;
    auto checked_scale = [&](VaSession &va, VASurfaceID source, VASurfaceID target, unsigned cx,
                             unsigned cy, unsigned cw, unsigned ch, unsigned tw, unsigned th) {
        auto before = engine_times_ns("vpe");
        // DRM fdinfo busy time is permitted to regress temporarily. If a short
        // job cannot be observed, repeat only the identical VPE operation. A
        // successful logical stage still requires an observed VPE increment;
        // unsupported paths cannot succeed through a CPU/shader fallback.
        bool refreshed=false;
        auto advanced = [&] {
            auto changed=[&](const auto &after) {
                for(const auto &[client,time]:after) {
                    auto old=before.find(client);
                    if(time>(old==before.end()?0:old->second))return true;
                }
                return false;
            };
            if(changed(engine_times_ns("vpe")))return true;
            // Discover a newly opened DRM client immediately on a miss. Normal
            // stages read only known DRM fds, not every RTSP/recording/socket fd.
            if(!refreshed){refreshed=true;return changed(engine_times_ns("vpe",true));}
            return false;
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        for (unsigned attempt = 0; attempt < 8; attempt++) {
            va.crop_scale(source, target, cx, cy, cw, ch, tw, th);
            auto observation_deadline =
                std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
            do {
                if (advanced())
                    return;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            } while (std::chrono::steady_clock::now() < observation_deadline);
            if (std::chrono::steady_clock::now() >= deadline)
                break;
        }
        throw std::runtime_error("Dedicated VPE execution could not be observed for " +
                                 std::to_string(cw) + "x" + std::to_string(ch) + " to " +
                                 std::to_string(tw) + "x" + std::to_string(th));
    };
    // Limit each VPE operation to a 4x ratio. Mesa's automatic geometric
    // downscale rejects some offset NV12 crops and asymmetric intermediate
    // rectangles. Explicit GPU-only stages avoid that path and reuse at most
    // two full-size VA intermediates; each stage has its own coefficient cache.
    unsigned cw = geometry.crop_width, ch = geometry.crop_height, stage = 0;
    const auto tw = geometry.scaled_width, th = geometry.scaled_height;
    while (cw > tw * 4 || ch > th * 4 || cw * 4 < tw || ch * 4 < th) {
        const unsigned iw = std::max(unsigned(video.width), s.info.width),
                       ih = std::max(unsigned(video.height), s.info.height);
        if (size_t(iw) * ih > 64 * 1024 * 1024)
            throw std::runtime_error("VPE intermediate budget exceeded");
        if (iw != s.intermediate_width || ih != s.intermediate_height) {
            s.clear_intermediate();
            s.intermediate_width = iw;
            s.intermediate_height = ih;
        }
        if (!s.intermediate_owner)
            s.intermediate_owner = std::make_unique<VaSession>(va_display);
        auto &target = s.intermediate[stage % 2];
        if (target == VA_INVALID_SURFACE)
            target = s.intermediate_owner->create_bgra(iw, ih);
        if (stage >= s.scale_contexts.size())
            s.scale_contexts.push_back(std::make_unique<VaSession>(va_display));
        const unsigned dw = std::clamp(tw, (cw + 3) / 4, cw * 4),
                       dh = std::clamp(th, (ch + 3) / 4, ch * 4);
        checked_scale(*s.scale_contexts[stage], input, target, x, y, cw, ch, dw, dh);
        input = target;
        x = y = 0;
        cw = dw;
        ch = dh;
        ++stage;
    }
    checked_scale(*s.va, input, s.surface, x, y, cw, ch, tw, th);
    if (!s.image)
        s.image = std::make_unique<ImportedSurface>(s.va->export_surface(s.surface));
    auto tensor = std::make_shared<Tensor>();
    tensor->device = s.device;
    tensor->geometry = geometry;
    tensor->width = geometry.width;
    tensor->height = geometry.height;
    tensor->original_width = geometry.original_width;
    tensor->original_height = geometry.original_height;
    tensor->scaled_width = geometry.scaled_width;
    tensor->scaled_height = geometry.scaled_height;
    tensor->pad_x = geometry.pad_x;
    tensor->pad_y = geometry.pad_y;
    if (destination) {
        // Keep the allocation alive even when a caller retains only buffer.
        tensor->buffer = std::shared_ptr<DeviceBuffer>(
            new DeviceBuffer(destination->device_data(), destination->descriptor().bytes()),
            [owner = destination](DeviceBuffer *view) { delete view; });
    } else
        tensor->buffer = std::make_shared<DeviceBuffer>(size_t(3) * s.info.width * s.info.height *
                                                        sizeof(float));
    struct Guard {
        ~Guard() { (void)hipDeviceSynchronize(); }
    } guard;
    s.pack.pack(*s.image, geometry, tensor->buffer->data(), s.info.normalization);
    hip_check(hipEventCreateWithFlags(&tensor->ready, hipEventDisableTiming), "Tensor event");
    hip_check(hipEventRecord(tensor->ready, nullptr), "Record tensor readiness");
    hip_check(hipEventSynchronize(tensor->ready), "Preprocessing completion");
    if (destination)
        destination->record_ready();
    return tensor;
}
} // namespace amdva
