#include "amdva/npu_pipeline.hpp"
#include "amdva/npu_meta.hpp"
#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <mutex>
namespace amdva {
struct NpuPipeline::Impl {
    GstElement *pipeline{}, *source{}, *sink{};
    GstBus *bus{};
    std::mutex mutex;
    uint64_t sequence{};
    bool failed{};
    ~Impl() {
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
        std::string text = "NPU pipeline reached EOS";
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error{};
            gchar *debug{};
            gst_message_parse_error(message, &error, &debug);
            text = error ? error->message : "NPU pipeline error";
            if (debug)
                text += std::string(": ") + debug;
            g_clear_error(&error);
            g_free(debug);
        }
        gst_message_unref(message);
        failed = true;
        throw std::runtime_error(text);
    }
};
NpuPipeline::NpuPipeline(const NpuModelConfig &config) : impl_(std::make_unique<Impl>()) {
    GError *error{};
    if (!gst_init_check(nullptr, nullptr, &error)) {
        std::string message = error ? error->message : "GStreamer initialization failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    auto &s = *impl_;
    s.pipeline = gst_pipeline_new(nullptr);
    // Add immediately so constructor failures release each element through the bin.
    auto make = [&](const char *factory) {
        auto *element = gst_element_factory_make(factory, nullptr);
        if (!element)
            throw std::runtime_error(std::string("Missing GStreamer element: ") + factory);
        gst_bin_add(GST_BIN(s.pipeline), element);
        return element;
    };
    s.source = make("appsrc");
    auto *npu = make("amdnpu");
    s.sink = make("appsink");
    g_object_set(npu, "model", config.model.c_str(), "provider-config",
                 config.provider_config.c_str(), "cache-dir", config.cache_dir.c_str(), "cache-key",
                 config.cache_key.c_str(), "gpu-device", config.gpu_device, nullptr);
    auto *caps = gst_caps_new_empty_simple("application/x-amd-shared-tensor");
    g_object_set(s.source, "caps", caps, "max-buffers", guint64(1), "block", TRUE, "format",
                 GST_FORMAT_TIME, "is-live", TRUE, nullptr);
    gst_caps_unref(caps);
    g_object_set(s.sink, "sync", FALSE, "async", FALSE, "max-buffers", guint(1), "drop", FALSE,
                 "enable-last-sample", FALSE, nullptr);
    if (!gst_element_link_many(s.source, npu, s.sink, nullptr))
        throw std::runtime_error("Cannot link NPU pipeline");
    s.bus = gst_element_get_bus(s.pipeline);
    if (gst_element_set_state(s.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        s.check_bus();
        throw std::runtime_error("Cannot start NPU pipeline");
    }
    s.check_bus();
}
NpuPipeline::~NpuPipeline() = default;
std::shared_ptr<NpuResult> NpuPipeline::infer(const std::shared_ptr<SharedTensor> &tensor) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed)
        throw std::runtime_error("NPU pipeline is failed; recreate it before retrying");
    s.check_bus();
    auto *buffer = gst_buffer_new();
    if (!add_shared_tensor_meta(buffer, tensor)) {
        gst_buffer_unref(buffer);
        throw std::invalid_argument("Cannot attach NPU input");
    }
    const auto sequence = ++s.sequence;
    GST_BUFFER_OFFSET(buffer) = sequence;
    GST_BUFFER_PTS(buffer) = sequence * GST_MSECOND;
    auto flow = gst_app_src_push_buffer(GST_APP_SRC(s.source), buffer);
    if (flow != GST_FLOW_OK) {
        s.failed = true;
        s.check_bus();
        throw std::runtime_error("NPU pipeline rejected input");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(s.sink), 100 * GST_MSECOND);
        if (sample) {
            auto *output = gst_sample_get_buffer(sample);
            auto *meta = get_npu_result_meta(output);
            std::shared_ptr<NpuResult> result;
            if (GST_BUFFER_OFFSET(output) == sequence && meta && meta->result)
                result = *meta->result;
            gst_sample_unref(sample);
            if (!result) {
                s.failed = true;
                throw std::runtime_error("Missing or mismatched NPU result");
            }
            return result;
        }
        s.check_bus();
    }
    s.failed = true;
    throw std::runtime_error("NPU inference timed out");
}
} // namespace amdva
