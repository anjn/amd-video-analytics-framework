#include "backend/reid.hpp"
#include "amdva/npu_meta.hpp"
#include "amdva/tensor_meta.hpp"
#include <algorithm>
#include <cmath>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
namespace amdva::nvr {
using Clock = std::chrono::steady_clock;
struct ReIdModel::Preprocessor {
    GstElement *pipeline{}, *source{}, *sink{};
    GstBus *bus{};
    uint64_t sequence{};
    bool failed{};
    std::string failure;
    ~Preprocessor() {
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus)
            gst_object_unref(bus);
        if (pipeline)
            gst_object_unref(pipeline);
    }
    void check_bus() {
        auto *message =
            gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message)
            return;
        std::string text = "ReID preprocessing reached EOS";
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error{};
            gchar *debug{};
            gst_message_parse_error(message, &error, &debug);
            text = error ? error->message : "ReID preprocessing failed";
            if (debug)
                text += std::string(": ") + debug;
            g_clear_error(&error);
            g_free(debug);
        }
        gst_message_unref(message);
        failed = true;
        failure = text;
        throw std::runtime_error(text);
    }
    void start(unsigned width, unsigned height, unsigned device) {
        pipeline = gst_pipeline_new(nullptr);
        auto make = [&](const char *factory) {
            auto *element = gst_element_factory_make(factory, nullptr);
            if (!element)
                throw std::runtime_error(std::string("ReID requires GStreamer element: ") +
                                         factory);
            gst_bin_add(GST_BIN(pipeline), element);
            return element;
        };
        source = make("appsrc");
        auto *pre = make("amdpreproc");
        sink = make("appsink");
        g_object_set(source, "max-buffers", guint64(1), "block", TRUE, "is-live", TRUE, "format",
                     GST_FORMAT_TIME, nullptr);
        g_object_set(pre, "device", int(device), "tensor-width", width, "tensor-height", height,
                     "roi-meta", TRUE, "letterbox", FALSE, "tensor-mean", "0.485,0.456,0.406",
                     "tensor-std", "0.229,0.224,0.225", nullptr);
        g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", guint(1), "drop", FALSE,
                     "enable-last-sample", FALSE, nullptr);
        if (!gst_element_link_many(source, pre, sink, nullptr))
            throw std::runtime_error("Cannot link ReID preprocessing pipeline");
        bus = gst_element_get_bus(pipeline);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            check_bus();
            throw std::runtime_error("Cannot start ReID preprocessing pipeline");
        }
        check_bus();
    }
    std::shared_ptr<Tensor> prepare(const ReIdInput &input) {
        if (failed)
            throw std::runtime_error(failure.empty() ? "ReID preprocessing pipeline failed"
                                                     : failure);
        check_bus();
        if (!input.frame)
            throw std::invalid_argument("ReID requires a VA video sample");
        auto *caps = gst_sample_get_caps(input.frame.get());
        auto *original = gst_sample_get_buffer(input.frame.get());
        GstVideoInfo video;
        if (!caps || !original || !gst_video_info_from_caps(&video, caps))
            throw std::invalid_argument("ReID requires a video sample with caps and buffer");
        auto box = search_crop(input.box, video.width, video.height);
        auto *buffer = gst_buffer_copy(original);
        // The request has exactly one ROI and a fresh tensor; source metadata stays untouched.
        for (auto api : {GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE, tensor_meta_api(),
                         shared_tensor_meta_api(), npu_result_meta_api()})
            while (auto *meta = gst_buffer_get_meta(buffer, api))
                gst_buffer_remove_meta(buffer, meta);
        gst_buffer_add_video_region_of_interest_meta(
            buffer, "person", unsigned(box.x), unsigned(box.y), unsigned(box.w), unsigned(box.h));
        auto seq = ++sequence;
        GST_BUFFER_OFFSET(buffer) = seq;
        auto *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
        gst_buffer_unref(buffer);
        auto flow = gst_app_src_push_sample(GST_APP_SRC(source), sample);
        gst_sample_unref(sample);
        if (flow != GST_FLOW_OK) {
            failed = true;
            check_bus();
            throw std::runtime_error("ReID preprocessing rejected input");
        }
        auto deadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < deadline) {
            auto *out = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
            if (out) {
                auto *b = gst_sample_get_buffer(out);
                auto *meta = get_tensor_meta(b);
                std::shared_ptr<Tensor> tensor;
                if (GST_BUFFER_OFFSET(b) == seq && meta && meta->tensor)
                    tensor = *meta->tensor;
                gst_sample_unref(out);
                if (!tensor) {
                    failed = true;
                    throw std::runtime_error("ReID preprocessing result missing or mismatched");
                }
                hip_check(hipEventSynchronize(tensor->ready), "ReID tensor readiness");
                return tensor;
            }
            check_bus();
        }
        failed = true;
        throw std::runtime_error("ReID preprocessing timed out");
    }
};
ReIdModel::ReIdModel(const std::string &path, unsigned device)
    : stream_(std::make_shared<HipStream>(device)), model_(path, device) {
    auto in = model_.inputs(), out = model_.outputs();
    if (in.size() != 1 || in[0].shape.size() != 4 || in[0].shape[1] != 3 || in[0].shape[0] < 1 ||
        in[0].shape[0] > 16 || in[0].shape[2] < 64 || in[0].shape[2] > 256 || in[0].shape[3] < 32 ||
        in[0].shape[3] > 128 || in[0].type != TensorType::Float32 || out.size() != 1 ||
        out[0].shape != std::vector<size_t>{in[0].shape[0], 512} ||
        out[0].type != TensorType::Float32)
        throw std::runtime_error("Invalid OSNet input/output contract");
    input_ = in[0];
    input_.storage = std::make_shared<DeviceBuffer>(input_.elements() * sizeof(float), stream_);
    preprocessor_ = std::make_unique<Preprocessor>();
    preprocessor_->start(input_.shape[3], input_.shape[2], device);
}
ReIdModel::~ReIdModel() = default;
ReIdFeature ReIdModel::encode(const ReIdInput &im) { return encode_many({im}).front(); }
std::vector<ReIdFeature> ReIdModel::encode_many(const std::vector<ReIdInput> &images) {
    hip_check(hipSetDevice(input_.device), "ReID device");
    prepare_ms = inference_ms = 0;
    std::vector<ReIdFeature> result;
    const unsigned batch = input_.shape[0];
    const size_t bytes = input_.elements() / batch * sizeof(float);
    for (size_t offset = 0; offset < images.size(); offset += batch) {
        auto start = Clock::now();
        unsigned count = std::min<size_t>(batch, images.size() - offset);
        std::vector<std::shared_ptr<Tensor>> tensors;
        for (unsigned n = 0; n < count; n++)
            tensors.push_back(preprocessor_->prepare(images[offset + n]));
        if (batch == 1)
            input_.storage = tensors.front()->buffer;
        else
            for (unsigned n = 0; n < batch; n++) {
                auto &tensor = tensors[std::min(n, count - 1)];
                hip_check(hipMemcpyAsync(static_cast<char *>(input_.storage->data()) + n * bytes,
                                         tensor->buffer->data(), bytes, hipMemcpyDeviceToDevice,
                                         stream_->get()),
                          "ReID GPU batch assembly");
            }
        // D2D copies may return before completion; MIGraphX can use another stream.
        stream_->finish();
        prepare_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        start = Clock::now();
        auto output = model_.infer({{input_.name, input_}});
        std::vector<ReIdFeature> features(batch);
        readback_metadata(features.data(), output[0].storage->data(),
                          features.size() * sizeof(ReIdFeature), stream_->get());
        inference_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        for (unsigned n = 0; n < count; n++) {
            auto &f = features[n];
            float norm = 0;
            for (float v : f) {
                if (!std::isfinite(v))
                    throw std::runtime_error("Non-finite ReID feature");
                norm += v * v;
            }
            if (norm < 1e-12f)
                throw std::runtime_error("Empty ReID feature");
            for (auto &v : f)
                v /= std::sqrt(norm);
            result.push_back(f);
        }
    }
    return result;
}
} // namespace amdva::nvr
