#include "amdva/tensor_meta.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>
#include <iostream>
using namespace amdva;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
using Sample = std::shared_ptr<GstSample>;
struct Pipeline {
    GstElement *pipeline{}, *source{}, *sink{};
    GstBus *bus{};
    Pipeline(bool normalize) {
        pipeline = gst_pipeline_new(nullptr);
        source = gst_element_factory_make("appsrc", nullptr);
        auto *pre = gst_element_factory_make("amdpreproc", nullptr);
        sink = gst_element_factory_make("appsink", nullptr);
        check(source && pre && sink, "missing preprocessing elements");
        gst_bin_add_many(GST_BIN(pipeline), source, pre, sink, nullptr);
        check(gst_element_link_many(source, pre, sink, nullptr), "preprocessing link");
        g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "max-buffers", guint64(1),
                     "block", TRUE, nullptr);
        g_object_set(pre, "roi-meta", TRUE, "letterbox", FALSE, "tensor-width", 128u,
                     "tensor-height", 256u, nullptr);
        if (normalize)
            g_object_set(pre, "tensor-mean", "0.485,0.456,0.406", "tensor-std", "0.229,0.224,0.225",
                         nullptr);
        g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, nullptr);
        bus = gst_element_get_bus(pipeline);
        check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
              "preprocessing start");
    }
    ~Pipeline() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
    }
    std::shared_ptr<Tensor> run(const Sample &frame, unsigned x, unsigned y, unsigned w, unsigned h,
                                int roi_count = 1) {
        auto *original = gst_sample_get_buffer(frame.get());
        auto *buffer = gst_buffer_copy(original);
        for (int i = 0; i < roi_count; i++)
            gst_buffer_add_video_region_of_interest_meta(buffer, "person", x, y, w, h);
        auto *sample = gst_sample_new(buffer, gst_sample_get_caps(frame.get()), nullptr, nullptr);
        gst_buffer_unref(buffer);
        check(gst_app_src_push_sample(GST_APP_SRC(source), sample) == GST_FLOW_OK,
              "preprocessing push");
        gst_sample_unref(sample);
        for (unsigned i = 0; i < 50; i++) {
            auto *out = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
            if (out) {
                auto *result = gst_sample_get_buffer(out);
                check(gst_buffer_peek_memory(result, 0) == gst_buffer_peek_memory(original, 0),
                      "VA payload copied");
                check(GST_BUFFER_PTS(result) == GST_BUFFER_PTS(original), "PTS changed");
                check(gst_caps_is_equal(gst_sample_get_caps(out), gst_sample_get_caps(frame.get())),
                      "caps changed");
                auto *meta = get_tensor_meta(result);
                check(meta && meta->tensor, "tensor metadata missing");
                auto tensor = *meta->tensor;
                gst_sample_unref(out);
                hip_check(hipEventSynchronize(tensor->ready), "tensor ready");
                return tensor;
            }
            auto *error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (error) {
                gst_message_unref(error);
                throw std::runtime_error("preprocessing bus error");
            }
        }
        throw std::runtime_error("preprocessing timeout");
    }
};
int main(int argc, char **argv) {
    try {
        gst_init(&argc, &argv);
        check(argc == 2, "Usage: preproc-roi-tests H264_FILE");
        auto *decode = gst_parse_launch(
            "filesrc name=file ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 "
            "! appsink name=sink sync=false max-buffers=1",
            nullptr);
        auto *file = gst_bin_get_by_name(GST_BIN(decode), "file");
        auto *sink = gst_bin_get_by_name(GST_BIN(decode), "sink");
        g_object_set(file, "location", argv[1], nullptr);
        gst_element_set_state(decode, GST_STATE_PLAYING);
        Sample frame(gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND),
                     gst_sample_unref);
        if (frame) {
            auto *buffer = gst_sample_get_buffer(frame.get());
            auto *display = gst_va_buffer_peek_display(buffer);
            va_check(vaSyncSurface(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)),
                                   gst_va_buffer_get_surface(buffer)),
                     "Fixture decode completion");
        }
        gst_element_set_state(decode, GST_STATE_NULL);
        gst_object_unref(file);
        gst_object_unref(sink);
        gst_object_unref(decode);
        check(bool(frame), "decode fixture missing");
        GstVideoInfo video;
        check(gst_video_info_from_caps(&video, gst_sample_get_caps(frame.get())), "video caps");
        unsigned x = video.width / 4 + 1, y = video.height / 4 + 1, w = video.width / 3,
                 h = video.height / 3;
        std::shared_ptr<Tensor> identity, normalized;
        {
            Pipeline a(false), b(true);
            identity = a.run(frame, x, y, w, h);
            normalized = b.run(frame, x, y, w, h);
            auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
            check(audit, "load copy audit with LD_PRELOAD");
            audit();
            // Change the ROI and return: the old tensor must retain its own allocation.
            auto next = b.run(frame, 0, 0, w, h);
            check(next->buffer->data() != normalized->buffer->data(), "queued tensor overwritten");
            auto again = b.run(frame, x, y, w, h);
            for (unsigned k = 0; k < 100; k++)
                b.run(frame, 0, 0, std::min(unsigned(video.width), 140u),
                      std::min(unsigned(video.height), 316u));
            const auto &g = again->geometry;
            check(g.crop_x == (x & ~1u) && g.crop_y == (y & ~1u), "ROI origin/chroma alignment");
            check(g.crop_width == ((x + w + 1) & ~1u) - (x & ~1u) &&
                      g.crop_height == ((y + h + 1) & ~1u) - (y & ~1u),
                  "ROI extent/chroma alignment");
            check(!g.pad_x && !g.pad_y && g.scaled_width == 128 && g.scaled_height == 256,
                  "ReID stretch geometry");
            check(again->buffer->size() == size_t(3) * 128 * 256 * sizeof(float), "tensor size");
        }
        // Compare normalization on the GPU; read back only two scalar diagnostics.
        Kernel compare(
            R"(extern "C" __global__ void compare(const float* rgb,const float* normalized,float* out) {
            if(blockIdx.x||threadIdx.x)return;
            const float mean[3]={.485f,.456f,.406f},sd[3]={.229f,.224f,.225f};
            float error=0,bad=0;for(unsigned i=0;i<3*128*256;i++) {
                float expected=(rgb[i]-mean[i/(128*256)])/sd[i/(128*256)];
                if(!isfinite(normalized[i]))bad+=1;
                error=fmaxf(error,fabsf(normalized[i]-expected));
            }out[0]=error;out[1]=bad;
        })",
            "compare", gpu_arch(0));
        DeviceBuffer diagnostics(2 * sizeof(float));
        auto *a = identity->buffer->data(), *b = normalized->buffer->data(),
             *out = diagnostics.data();
        void *args[] = {&a, &b, &out};
        compare.launch(1, args);
        float result[2];
        readback_metadata(result, out, sizeof(result));
        check(result[0] < 1e-5 && result[1] == 0, "ImageNet normalization mismatch");
        for (int kind = 0; kind < 3; kind++) {
            Pipeline p(false);
            bool rejected = false;
            try {
                p.run(frame, x, y, kind == 2 ? unsigned(video.width) : w, h,
                      kind == 0   ? 0
                      : kind == 1 ? 2
                                  : 1);
            } catch (const std::exception &e) {
                rejected = std::string(e.what()) == "preprocessing bus error";
            }
            check(rejected, "missing/multiple/out-of-bounds ROI accepted");
        }
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        check(errors && errors() == 0, "copy audit violation");
        std::cout << "{\"passed\":true,\"roi_alignment\":true,\"va_memory_identity\":true,"
                     "\"normalization_max_error\":"
                  << result[0] << ",\"invalid_roi_rejected\":true,\"audit_errors\":0}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
