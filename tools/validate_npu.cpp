#include "amdva/npu_meta.hpp"
#include "amdva/npu_pipeline.hpp"
#include "amdva/tensor_meta.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
using namespace amdva;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> static void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "invalid input was accepted");
}
static void verify(const NpuResult &result, float fill, const SharedTensor *input = nullptr) {
    check(result.tensors.size() == 2, "two outputs required");
    if (input)
        check(result.input_address == reinterpret_cast<uintptr_t>(input->host_data()),
              "input pointer changed");
    for (size_t i = 0; i < 2; ++i) {
        const auto &tensor = result.tensors[i];
        check(tensor.descriptor.name == (i == 0 ? "features" : "pooled"), "output name");
        check(tensor.descriptor.type == TensorType::Float32, "output type");
        check(tensor.descriptor.shape ==
                  (i == 0 ? std::vector<size_t>{1, 8, 32, 32} : std::vector<size_t>{1, 8, 1, 1}),
              "output shape");
        check(tensor.owner && tensor.data, "output owner");
        auto *data = static_cast<const float *>(tensor.data);
        for (size_t k = 0; k < tensor.descriptor.bytes() / sizeof(float); ++k)
            check(std::isfinite(data[k]) && std::abs(data[k] - (.5f + .75f * fill)) < .02f,
                  "output parity");
    }
}
static void fill(const std::shared_ptr<SharedTensor> &input, bool one) {
    // GPU kernel writes the allocation ORT will bind; no tensor upload/readback.
    struct Stream {
        hipStream_t value{};
        Stream() {
            hip_check(hipStreamCreateWithFlags(&value, hipStreamNonBlocking), "Producer stream");
        }
        ~Stream() { (void)hipStreamDestroy(value); }
    };
    static Stream stream;
    static Kernel kernel(R"(extern "C" __global__ void fill(float* x, unsigned n, float v) {
        unsigned i=blockIdx.x*blockDim.x+threadIdx.x; if(i<n)x[i]=v;
    })",
                         "fill", gpu_arch(0));
    auto *data = input->device_data();
    unsigned n = input->descriptor().bytes() / sizeof(float);
    float value = one ? 1.f : 0.f;
    void *args[] = {&data, &n, &value};
    kernel.launch(n, args, stream.value);
    input->record_ready(stream.value);
}
struct Pipeline {
    GstElement *pipeline{}, *source{}, *npu{}, *sink{};
    GstBus *bus{};
    explicit Pipeline(const NpuModelConfig &c) {
        pipeline = gst_pipeline_new(nullptr);
        source = gst_element_factory_make("appsrc", nullptr);
        npu = gst_element_factory_make("amdnpu", nullptr);
        sink = gst_element_factory_make("appsink", nullptr);
        check(source && npu && sink, "NPU test elements missing");
        gst_bin_add_many(GST_BIN(pipeline), source, npu, sink, nullptr);
        check(gst_element_link_many(source, npu, sink, nullptr), "link failed");
        auto *caps = gst_caps_from_string("application/x-amd-shared-tensor");
        g_object_set(source, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, nullptr);
        gst_caps_unref(caps);
        g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, nullptr);
        g_object_set(npu, "model", c.model.c_str(), "provider-config", c.provider_config.c_str(),
                     "cache-dir", c.cache_dir.c_str(), "cache-key", c.cache_key.c_str(), nullptr);
        bus = gst_element_get_bus(pipeline);
    }
    ~Pipeline() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
    }
    void start() {
        check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
              "start failed");
    }
    GstBuffer *push(const std::shared_ptr<SharedTensor> &input) {
        auto *buffer = gst_buffer_new_allocate(nullptr, 8, nullptr);
        GST_BUFFER_PTS(buffer) = 9 * GST_SECOND;
        add_shared_tensor_meta(buffer, input);
        auto *copy = gst_buffer_copy(buffer);
        auto *region = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_ALL, 1, 2);
        check(!get_shared_tensor_meta(region), "region retained input");
        gst_buffer_unref(region);
        gst_buffer_unref(buffer);
        check(get_shared_tensor_meta(copy) && *get_shared_tensor_meta(copy)->tensor == input,
              "copy lost allocation");
        check(gst_app_src_push_buffer(GST_APP_SRC(source), copy) == GST_FLOW_OK, "push failed");
        auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        check(sample, "sample missing");
        auto *output = gst_buffer_ref(gst_sample_get_buffer(sample));
        gst_sample_unref(sample);
        check(GST_BUFFER_PTS(output) == 9 * GST_SECOND, "PTS changed");
        check(get_shared_tensor_meta(output) && *get_shared_tensor_meta(output)->tensor == input,
              "output lost input allocation");
        check(get_npu_result_meta(output), "result missing");
        return output;
    }
};
static void video(const NpuModelConfig &c, const char *file) {
    struct Original {
        GstMemory *memory{};
        GstClockTime pts{};
        ~Original() {
            if (memory)
                gst_memory_unref(memory);
        }
    } original;
    // Exercise VA decode -> existing HIP preprocessing -> mapped tensor -> NPU.
    GError *error{};
    auto *p = gst_parse_launch(
        "filesrc name=file ! qtdemux ! h265parse ! vah265dec ! "
        "amdpreproc name=pre output-memory=host-mapped tensor-width=32 tensor-height=32 ! "
        "amdnpu name=npu ! appsink name=sink sync=false max-buffers=1",
        &error);
    if (error) {
        std::string msg = error->message;
        g_error_free(error);
        if (p)
            gst_object_unref(p);
        throw std::runtime_error(msg);
    }
    auto *src = gst_bin_get_by_name(GST_BIN(p), "file");
    auto *pre = gst_bin_get_by_name(GST_BIN(p), "pre");
    auto *pad = gst_element_get_static_pad(pre, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto &saved = *static_cast<Original *>(data);
            if (!saved.memory) {
                auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                saved.memory = gst_memory_ref(gst_buffer_peek_memory(buffer, 0));
                saved.pts = GST_BUFFER_PTS(buffer);
            }
            return GST_PAD_PROBE_OK;
        },
        &original, nullptr);
    gst_object_unref(pad);
    gst_object_unref(pre);
    auto *npu = gst_bin_get_by_name(GST_BIN(p), "npu");
    auto *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    g_object_set(src, "location", file, nullptr);
    g_object_set(npu, "model", c.model.c_str(), "provider-config", c.provider_config.c_str(),
                 "cache-dir", c.cache_dir.c_str(), "cache-key", c.cache_key.c_str(), nullptr);
    gst_element_set_state(p, GST_STATE_PLAYING);
    auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND);
    auto *bus = gst_element_get_bus(p);
    auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    std::string failure;
    if (message) {
        GError *e{};
        gchar *debug{};
        gst_message_parse_error(message, &e, &debug);
        failure = e->message;
        if (debug)
            failure += std::string(": ") + debug;
        g_clear_error(&e);
        g_free(debug);
        gst_message_unref(message);
    }
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(npu);
    gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(p);
    if (!failure.empty()) {
        if (sample)
            gst_sample_unref(sample);
        throw std::runtime_error(failure);
    }
    check(sample, "VA preprocessing sample missing");
    auto *buffer = gst_sample_get_buffer(sample);
    check(gst_buffer_n_memory(buffer) > 0, "VA payload missing");
    check(gst_buffer_peek_memory(buffer, 0) == original.memory, "VA payload changed");
    check(GST_BUFFER_PTS(buffer) == original.pts, "VA PTS changed");
    auto *in = get_shared_tensor_meta(buffer);
    auto *out = get_npu_result_meta(buffer);
    check(in && out, "VA preprocessing metadata missing");
    check((*out->result)->input_address == reinterpret_cast<uintptr_t>((*in->tensor)->host_data()),
          "VA tensor pointer changed");
    check(!get_tensor_meta(buffer), "device-only tensor attached to mapped path");
    gst_sample_unref(sample);
}
int main(int argc, char **argv) {
    try {
        gst_init(&argc, &argv);
        check(argc == 2 || argc == 3, "Usage: amdva-validate-npu FIXTURE_DIR [HEVC_MP4]");
        auto root = std::filesystem::absolute(argv[1]);
        NpuModelConfig config{(root / "image.onnx").string(), (root / "provider.json").string(),
                              (root / "cache").string(), "fixture", 0};
        auto input = std::make_shared<SharedTensor>(
            TensorDescriptor{"images", TensorType::Float32, {1, 3, 32, 32}});
        {
            auto temporary = std::make_shared<SharedTensor>(input->descriptor());
            std::weak_ptr<SharedTensor> weak = temporary;
            auto *buffer = gst_buffer_new();
            check(add_shared_tensor_meta(buffer, temporary), "input attachment");
            auto *copy = gst_buffer_copy(buffer);
            temporary.reset();
            gst_buffer_unref(buffer);
            check(!weak.expired(), "copied input allocation freed early");
            gst_buffer_unref(copy);
            check(weak.expired(), "copied input allocation leaked");
        }
        rejects([&] { input->wait_ready(); });
        {
            NpuExecutor executor(config);
            rejects([&] { executor.infer(input); });
            for (auto desc : {TensorDescriptor{"wrong", TensorType::Float32, {1, 3, 32, 32}},
                              {"images", TensorType::Float16, {1, 3, 32, 32}},
                              {"images", TensorType::Float32, {1, 3, 16, 16}}}) {
                auto bad = std::make_shared<SharedTensor>(desc);
                rejects([&] { executor.infer(bad); });
            }
        }
        auto invalid = config;
        invalid.model = "/missing/model.onnx";
        rejects([&] { NpuPipeline bad(invalid); });
        std::shared_ptr<NpuResult> retained;
        {
            NpuPipeline pipeline(config);
            for (int i = 0; i < 100; ++i) {
                fill(input, i % 2);
                auto result = pipeline.infer(input);
                verify(*result, float(i % 2), input.get());
                if (i == 0)
                    retained = result;
                verify(*retained, 0);
            }
        }
        verify(*retained, 0); // Output survives pipeline/session stop and later Run calls.
        {
            Pipeline pipeline(config);
            auto *pad = gst_element_get_static_pad(pipeline.npu, "sink");
            auto *caps = gst_pad_query_caps(pad, nullptr);
            auto *host = gst_caps_from_string("video/x-raw,format=NV12,width=32,height=32");
            check(!gst_caps_can_intersect(caps, host), "plain host video accepted");
            gst_caps_unref(caps);
            gst_caps_unref(host);
            gst_object_unref(pad);
            pipeline.start();
            g_object_set(pipeline.npu, "model", "/ignored/while-playing", nullptr);
            gchar *model{};
            g_object_get(pipeline.npu, "model", &model, nullptr);
            check(config.model == model, "PLAYING property mutation accepted");
            g_free(model);
            fill(input, false);
            auto *first = pipeline.push(input);
            auto *copy = gst_buffer_copy(first);
            gst_buffer_unref(first);
            auto output = *get_npu_result_meta(copy)->result;
            // Flush an idle pipeline, then resume with a new segment and buffer.
            check(gst_element_send_event(pipeline.pipeline, gst_event_new_flush_start()),
                  "flush start");
            check(gst_element_send_event(pipeline.pipeline, gst_event_new_flush_stop(TRUE)),
                  "flush stop");
            auto next = std::make_shared<SharedTensor>(input->descriptor());
            fill(next, true);
            auto *second = pipeline.push(next);
            gst_buffer_unref(second);
            verify(*output, 0);
            gst_app_src_end_of_stream(GST_APP_SRC(pipeline.source));
            auto *eos = gst_bus_timed_pop_filtered(
                pipeline.bus, 5 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            check(eos && GST_MESSAGE_TYPE(eos) == GST_MESSAGE_EOS, "EOS failed");
            gst_message_unref(eos);
            gst_element_set_state(pipeline.pipeline, GST_STATE_NULL);
            verify(*output, 0);
            gst_buffer_unref(copy);
        }
        {
            Pipeline pipeline(config);
            pipeline.start();
            gst_app_src_push_buffer(GST_APP_SRC(pipeline.source), gst_buffer_new());
            auto *message =
                gst_bus_timed_pop_filtered(pipeline.bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
            check(message, "missing shared meta did not post bus error");
            gst_message_unref(message);
        }
        if (argc == 3)
            video(config, argv[2]);
        std::cout
            << "{\"passed\":true,\"iterations\":100,\"outputs\":2,\"input_pointer_equal\":true,"
               "\"retained_output_after_stop\":true,\"metadata_copy\":true,\"flush_eos\":true,"
               "\"invalid_inputs_rejected\":true,\"video_preprocessing\":"
            << (argc == 3 ? "true" : "false") << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
