#include "amdva/npu.hpp"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <unistd.h>
namespace amdva {
namespace {
TensorType type(ONNXTensorElementDataType v) {
    switch (v) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        return TensorType::Float32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
        return TensorType::Float16;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
        return TensorType::Int32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
        return TensorType::Int64;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
        return TensorType::UInt8;
    default:
        throw std::runtime_error("Unsupported NPU tensor type");
    }
}
template <class T> TensorDescriptor describe(const char *name, const T &info) {
    TensorDescriptor d{name, type(info.GetElementType()), {}};
    for (auto n : info.GetShape()) {
        if (n <= 0)
            throw std::runtime_error("NPU requires static positive tensor dimensions");
        d.shape.push_back(size_t(n));
    }
    d.bytes();
    return d;
}
struct OutputStorage {
    // Keep session/environment alive if downstream retains a tensor after stop.
    std::shared_ptr<void> runtime;
    std::vector<Ort::Value> values;
};
} // namespace
struct NpuExecutor::Impl {
    NpuModelConfig config;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "amdva-npu"};
    std::unique_ptr<Ort::Session> session;
    TensorDescriptor input;
    std::vector<TensorDescriptor> outputs;
    std::mutex mutex;
    bool profiling{};
    ~Impl() {
        if (profiling && session) {
            try {
                Ort::AllocatorWithDefaultOptions allocator;
                auto path = session->EndProfilingAllocated(allocator);
                std::clog << "NPU ORT profile: " << path.get() << '\n';
            } catch (const std::exception &e) {
                std::clog << "NPU profiling failed: " << e.what() << '\n';
            }
        }
    }
    explicit Impl(const NpuModelConfig &c) : config(c) {
        namespace fs = std::filesystem;
        if (!fs::is_regular_file(c.model) || !fs::is_regular_file(c.provider_config) ||
            c.cache_key.empty() ||
            c.cache_key.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            !fs::is_regular_file(fs::path(c.cache_dir) / c.cache_key / "context.json"))
            throw std::invalid_argument(
                "NPU requires model, provider-config and a prepared cache-dir/cache-key");
        hip_check(hipSetDevice(c.gpu_device), "NPU producer GPU device");
        Ort::SessionOptions options;
        if (const char *prefix = std::getenv("AMDVA_NPU_PROFILE_PREFIX"); prefix && *prefix) {
            static std::atomic<unsigned> sequence{0};
            auto unique = std::string(prefix) + "-" + std::to_string(getpid()) + "-" +
                          std::to_string(sequence++);
            options.EnableProfiling(unique.c_str());
            profiling = true;
        }
        options.SetIntraOpNumThreads(4);
        options.AddFreeDimensionOverrideByName("batch", 1);
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        try {
            options.AppendExecutionProvider_VitisAI({{"config_file", c.provider_config},
                                                     {"cache_dir", c.cache_dir},
                                                     {"cache_key", c.cache_key},
                                                     {"enable_cache_file_io_in_mem", "0"}});
            session = std::make_unique<Ort::Session>(env, c.model.c_str(), options);
        } catch (const Ort::Exception &e) {
            throw std::runtime_error(std::string("Vitis AI EP initialization failed: ") + e.what() +
                                     ". Source the Ryzen AI environment before starting GStreamer; "
                                     "NVR users can use examples/nvr/scripts/run.sh.");
        }
        if (session->GetInputCount() != 1 || session->GetOutputCount() == 0)
            throw std::runtime_error("NPU requires exactly one image input and numeric outputs");
        Ort::AllocatorWithDefaultOptions allocator;
        auto name = session->GetInputNameAllocated(0, allocator);
        auto input_info = session->GetInputTypeInfo(0);
        input = describe(name.get(), input_info.GetTensorTypeAndShapeInfo());
        if (input.type != TensorType::Float32 || input.shape.size() != 4 || input.shape[0] != 1 ||
            input.shape[1] != 3)
            throw std::runtime_error("NPU input must be static float32 [1,3,H,W]");
        for (size_t i = 0; i < session->GetOutputCount(); i++) {
            auto output_name = session->GetOutputNameAllocated(i, allocator);
            auto output_info = session->GetOutputTypeInfo(i);
            outputs.push_back(describe(output_name.get(), output_info.GetTensorTypeAndShapeInfo()));
        }
    }
};
NpuExecutor::NpuExecutor(const NpuModelConfig &c) : impl_(std::make_shared<Impl>(c)) {}
NpuExecutor::~NpuExecutor() = default;
const TensorDescriptor &NpuExecutor::input() const { return impl_->input; }
const std::vector<TensorDescriptor> &NpuExecutor::outputs() const { return impl_->outputs; }
std::shared_ptr<NpuResult> NpuExecutor::infer(const std::shared_ptr<SharedTensor> &tensor) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!tensor || tensor->descriptor().name != s.input.name ||
        tensor->descriptor().type != s.input.type || tensor->descriptor().shape != s.input.shape ||
        tensor->device() != s.config.gpu_device)
        throw std::invalid_argument("NPU shared input name, type, shape or GPU device mismatch");
    tensor->wait_ready();
    std::vector<int64_t> shape(s.input.shape.begin(), s.input.shape.end());
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input = Ort::Value::CreateTensor<float>(memory, static_cast<float *>(tensor->host_data()),
                                                 s.input.bytes() / sizeof(float), shape.data(),
                                                 shape.size());
    if (input.GetTensorData<float>() != tensor->host_data())
        throw std::runtime_error("ORT changed the shared input pointer");
    const char *input_name = s.input.name.c_str();
    std::vector<const char *> names;
    for (auto &out : s.outputs)
        names.push_back(out.name.c_str());
    auto storage = std::make_shared<OutputStorage>();
    storage->runtime = impl_;
    storage->values = s.session->Run(Ort::RunOptions{nullptr}, &input_name, &input, 1, names.data(),
                                     names.size());
    auto result = std::make_shared<NpuResult>();
    result->input_address = reinterpret_cast<uintptr_t>(tensor->host_data());
    for (size_t i = 0; i < s.outputs.size(); i++) {
        auto actual = storage->values[i].GetTensorTypeAndShapeInfo();
        auto desc = describe(s.outputs[i].name.c_str(), actual);
        if (desc.type != s.outputs[i].type || desc.shape != s.outputs[i].shape)
            throw std::runtime_error("NPU output changed shape/type");
        result->tensors.push_back({desc, storage->values[i].GetTensorRawData(), storage});
    }
    return result;
}
} // namespace amdva
