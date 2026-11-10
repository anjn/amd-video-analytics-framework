// Bundle/text conventions and the Pillow-compatible filter are adapted from
// embeddinggemma2-rocm 102a9df65cd312529668daabb3e88e0a17cfc363 (MIT).
// Copyright (c) 2026 embeddinggemma2-rocm contributors.
// License: third_party/mobileclip-tokenizer/LICENSE.
#include <algorithm>
#include <amdva/mobileclip.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <json-glib/json-glib.h>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <stdexcept>

extern "C" void *eg2_tokenizer_new(const char *, char *, size_t);
extern "C" void eg2_tokenizer_free(void *);
extern "C" int mc2_tokenizer_encode(const void *, const char *const *, size_t, int64_t *, char *,
                                    size_t);
extern "C" void amdva_audit_mobileclip_input(bool) __attribute__((weak));
extern "C" void amdva_audit_embedding_scope(bool) __attribute__((weak));
namespace fs = std::filesystem;
namespace amdva::mobileclip {
namespace {
constexpr size_t elements = 3 * 256 * 256;
struct Json {
    JsonParser *p = json_parser_new();
    explicit Json(const fs::path &path) {
        GError *e = nullptr;
        if (!json_parser_load_from_file(p, path.c_str(), &e)) {
            std::string message = e ? e->message : "JSON read failed";
            g_clear_error(&e);
            g_object_unref(p);
            p = nullptr;
            throw std::runtime_error(message);
        }
        if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            g_object_unref(p);
            p = nullptr;
            throw std::runtime_error("Expected bundle JSON object");
        }
    }
    ~Json() {
        if (p)
            g_object_unref(p);
    }
    JsonObject *get() const { return json_node_get_object(json_parser_get_root(p)); }
};
std::string json_text(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) ||
        json_node_get_value_type(json_object_get_member(o, key)) != G_TYPE_STRING)
        throw std::runtime_error(std::string("Missing string: ") + key);
    return json_object_get_string_member(o, key);
}
std::string digest(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read " + path.string());
    auto sum = std::unique_ptr<GChecksum, decltype(&g_checksum_free)>(
        g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    char bytes[65536];
    while (f) {
        f.read(bytes, sizeof(bytes));
        g_checksum_update(sum.get(), reinterpret_cast<guchar *>(bytes), f.gcount());
    }
    if (!f.eof())
        throw std::runtime_error("Read failed: " + path.string());
    return g_checksum_get_string(sum.get());
}
void artifacts(const fs::path &root, JsonObject *meta) {
    if (!json_object_has_member(meta, "artifacts") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(meta, "artifacts")))
        throw std::runtime_error("Missing artifact manifest");
    auto obj = json_object_get_object_member(meta, "artifacts");
    auto members = json_object_get_members(obj);
    try {
        for (auto it = members; it; it = it->next) {
            auto name = static_cast<const char *>(it->data);
            fs::path relative(name);
            if (relative.is_absolute() || std::any_of(relative.begin(), relative.end(),
                                                      [](const auto &v) { return v == ".."; }))
                throw std::runtime_error("Invalid artifact path");
            if (digest(root / relative) != json_text(obj, name))
                throw std::runtime_error("Artifact checksum mismatch: " + relative.string());
        }
    } catch (...) {
        g_list_free(members);
        throw;
    }
    g_list_free(members);
}
void normalize(std::array<float, 512> &y) {
    double sum = 0;
    for (float v : y) {
        if (!std::isfinite(v))
            throw std::runtime_error("Non-finite embedding");
        sum += double(v) * v;
    }
    if (sum <= 0)
        throw std::runtime_error("Zero embedding");
    float scale = float(1 / std::sqrt(sum));
    for (auto &v : y)
        v *= scale;
}
struct InputScope {
    InputScope() {
        if (amdva_audit_mobileclip_input)
            amdva_audit_mobileclip_input(true);
    }
    ~InputScope() {
        if (amdva_audit_mobileclip_input)
            amdva_audit_mobileclip_input(false);
    }
};
// Exactly the upstream Pillow-compatible 22-bit, separable uint8 bilinear filter.
// Coefficients are computed on the GPU; no image or tensor upload is involved.
const char *source = R"(
__device__ double weight(int k,double center,double support) {
 return fmax(0.0,1.0-fabs((k-center+0.5)/support));
}
extern "C" __global__ void horizontal(const unsigned char *rgb,int w,int h,unsigned char *tmp) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
 int side=max(w,h); if(i>=unsigned(side*256*3))return;
 int c=i%3,x=(i/3)%256,y=i/(3*256),ox=(side-w)/2,oy=(side-h)/2;
 if(y<oy||y>=oy+h){tmp[i]=0;return;}
 double scale=double(side)/256.0,support=fmax(1.0,scale),center=(x+0.5)*scale;
 int lo=max(0,int(center-support+0.5)),hi=min(side,int(center+support+0.5));
 double sum=0;for(int k=lo;k<hi;k++)sum+=weight(k,center,support);
 long long v=1<<21;
 for(int k=lo;k<hi;k++)if(k>=ox&&k<ox+w){
  int coeff=int(weight(k,center,support)/sum*(1<<22)+0.5);
  v+=((long long)rgb[((y-oy)*w+k-ox)*3+c])*coeff;
 }
 tmp[i]=(unsigned char)min(255ll,max(0ll,v>>22));
}
extern "C" __global__ void vertical(const unsigned char *tmp,int side,float *dst) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=3*256*256)return;
 int x=i%256,y=(i/256)%256,c=i/(256*256);
 double scale=double(side)/256.0,support=fmax(1.0,scale),center=(y+0.5)*scale;
 int lo=max(0,int(center-support+0.5)),hi=min(side,int(center+support+0.5));
 double sum=0;for(int k=lo;k<hi;k++)sum+=weight(k,center,support);
 long long v=1<<21;
 for(int k=lo;k<hi;k++){
  int coeff=int(weight(k,center,support)/sum*(1<<22)+0.5);
  v+=((long long)tmp[(k*256+x)*3+c])*coeff;
 }
 dst[i]=float(min(255ll,max(0ll,v>>22)))/255.0f;
}
)";
struct Memory {
    std::shared_ptr<DeviceBuffer> gpu;
    void *host = nullptr, *device = nullptr;
    size_t bytes;
    int ordinal;
    Memory(bool npu, unsigned batch, int d) : bytes(batch * elements * sizeof(float)), ordinal(d) {
        hip_check(hipSetDevice(d), "MobileCLIP allocation device");
        if (npu) {
            hip_check(hipHostMalloc(&host, bytes, hipHostMallocMapped),
                      "MobileCLIP shared input allocation");
            try {
                hip_check(hipHostGetDevicePointer(&device, host, 0),
                          "MobileCLIP shared device pointer");
            } catch (...) {
                (void)hipHostFree(host);
                host = nullptr;
                throw;
            }
        } else {
            gpu = std::make_shared<DeviceBuffer>(bytes);
            device = gpu->data();
        }
    }
    ~Memory() {
        (void)hipSetDevice(ordinal);
        if (host)
            (void)hipHostFree(host);
    }
};
} // namespace
struct PreparedInput::Impl {
    std::shared_ptr<Memory> memory;
    const void *owner{};
    Evidence evidence;
    std::vector<Image> images;
};
PreparedInput::PreparedInput() = default;
PreparedInput::~PreparedInput() = default;
PreparedInput::PreparedInput(PreparedInput &&) noexcept = default;
PreparedInput &PreparedInput::operator=(PreparedInput &&) noexcept = default;
Evidence PreparedInput::evidence() const {
    if (!impl_)
        throw std::logic_error("Empty input");
    return impl_->evidence;
}
std::vector<float> PreparedInput::diagnostic_pixels() const {
    auto e = evidence();
    std::vector<float> result(e.input_bytes / sizeof(float));
    hip_check(hipSetDevice(impl_->memory->ordinal), "Diagnostic device");
    // Deliberate readback outside input handoff, for sample/reference checks only.
    hip_check(hipMemcpy(result.data(), impl_->memory->device, e.input_bytes, hipMemcpyDeviceToHost),
              "Diagnostic pixels");
    return result;
}
struct Runtime::Impl {
    fs::path root;
    std::string backend;
    int device;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "amdva-mobileclip"};
    std::unique_ptr<Ort::Session> image, text_session;
    std::unique_ptr<void, decltype(&eg2_tokenizer_free)> tokenizer{nullptr, eg2_tokenizer_free};
    std::map<unsigned, std::unique_ptr<ModelExecutor>> models;
    std::unique_ptr<Kernel> horizontal, vertical;
    std::vector<std::shared_ptr<Memory>> pool;
    std::mutex mutex;
    uint64_t sequence{};
    Impl(fs::path r, std::string b, int d)
        : root(fs::absolute(r)), backend(std::move(b)), device(d) {
        if (backend != "gpu" && backend != "npu")
            throw std::invalid_argument("Expected gpu or npu");
        Json manifest(root / "manifest.json");
        if (json_text(manifest.get(), "model") != "MobileCLIP2-S2" ||
            json_text(manifest.get(), "preprocessing") != "letterbox-bilinear-256" ||
            json_object_get_int_member(manifest.get(), "format") != 1 ||
            json_object_get_int_member(manifest.get(), "dimension") != 512)
            throw std::runtime_error("Incompatible MobileCLIP bundle");
        for (const char *name : {"image", "text", "tokenizer"}) {
            auto file =
                root / (std::string(name) + (std::string(name) == "tokenizer" ? ".json" : ".onnx"));
            if (digest(file) != json_text(manifest.get(), (std::string(name) + "_sha256").c_str()))
                throw std::runtime_error("Bundle checksum mismatch");
        }
        Json meta(root / (backend == "gpu" ? "gpu-device.json" : "npu.json"));
        if (json_text(meta.get(), "image_sha256") != json_text(manifest.get(), "image_sha256"))
            throw std::runtime_error("Compiled model ONNX mismatch");
        artifacts(root, meta.get());
        if (backend == "npu") {
            if (json_text(meta.get(), "onnxruntime") != Ort::GetVersionString())
                throw std::runtime_error("NPU cache ORT version mismatch");
            auto key = json_text(meta.get(), "cache_key");
            if (key.empty() ||
                key.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos ||
                !fs::exists(root / "npu-cache" / key / "context.json"))
                throw std::runtime_error("Missing or invalid NPU cache");
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(4);
            opts.AddFreeDimensionOverrideByName("batch", 1);
            opts.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
            try {
                opts.AppendExecutionProvider_VitisAI(
                    {{"config_file", (root / "npu-config.json").string()},
                     {"cache_dir", (root / "npu-cache").string()},
                     {"cache_key", key},
                     {"enable_cache_file_io_in_mem", "0"}});
                image = std::make_unique<Ort::Session>(env, (root / "image.onnx").c_str(), opts);
            } catch (const Ort::Exception &e) {
                throw std::runtime_error(std::string("MobileCLIP NPU initialization failed: ") +
                    e.what() + ". Launch NVR with examples/nvr/scripts/run.sh CONFIG.json "
                    "and set search.ryzen_ai_venv (or AMDVA_RYZEN_AI_VENV) to the Ryzen AI venv. "
                    "ReID cannot start until search initialization succeeds.");
            }
        } else {
            for (unsigned batch : {1u, 4u}) {
                auto model = std::make_unique<ModelExecutor>(
                    (root / ("gpu-device-b" + std::to_string(batch) + ".mxr")).string(), device);
                auto in = model->inputs(), out = model->outputs();
                if (in.size() != 1 || in[0].name != "images" || in[0].type != TensorType::Float32 ||
                    in[0].shape != std::vector<size_t>{batch, 3, 256, 256} || out.size() != 1 ||
                    out[0].shape != std::vector<size_t>{batch, 512} ||
                    out[0].type != TensorType::Float32)
                    throw std::runtime_error("Invalid GPU MobileCLIP shape/type");
                models.emplace(batch, std::move(model));
            }
        }
        char error[4096]{};
        tokenizer.reset(eg2_tokenizer_new((root / "tokenizer.json").c_str(), error, sizeof(error)));
        if (!tokenizer)
            throw std::runtime_error(error);
        Ort::SessionOptions text_opts;
        text_opts.SetIntraOpNumThreads(4);
        text_opts.SetInterOpNumThreads(1);
        text_session = std::make_unique<Ort::Session>(env, (root / "text.onnx").c_str(), text_opts);
        horizontal = std::make_unique<Kernel>(source, "horizontal", gpu_arch(device));
        vertical = std::make_unique<Kernel>(source, "vertical", gpu_arch(device));
    }
};
Runtime::Runtime(const std::string &bundle, const std::string &backend, int device)
    : impl_(std::make_unique<Impl>(bundle, backend, device)) {}
Runtime::~Runtime() = default;
Image upload(const uint8_t *rgb, unsigned w, unsigned h, int device) {
    if (!rgb || !w || !h || w > 1024 || h > 1024)
        throw std::invalid_argument("Invalid RGB image");
    hip_check(hipSetDevice(device), "Image upload device");
    Image result{w, h, std::make_shared<DeviceBuffer>(size_t(w) * h * 3), device};
    // Explicit CPU-source ingestion, outside the copy-free GPU-input contract.
    if (amdva_audit_embedding_scope)
        amdva_audit_embedding_scope(true);
    auto rc = hipMemcpy(result.storage->data(), rgb, result.storage->size(), hipMemcpyHostToDevice);
    if (amdva_audit_embedding_scope)
        amdva_audit_embedding_scope(false);
    hip_check(rc, "CPU image ingestion");
    return result;
}
PreparedInput Runtime::prepare(const std::vector<Image> &images) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (images.empty() || images.size() > (s.backend == "npu" ? 1u : 4u))
        throw std::invalid_argument("Invalid MobileCLIP batch");
    hip_check(hipSetDevice(s.device), "MobileCLIP prepare device");
    for (const auto &im : images) {
        if (!im.width || !im.height || im.width > 1024 || im.height > 1024 || !im.storage ||
            im.storage->size() != size_t(im.width) * im.height * 3 || im.device != s.device)
            throw std::invalid_argument("Invalid GPU RGB image");
        hipPointerAttribute_t attr{};
        hip_check(hipPointerGetAttributes(&attr, im.storage->data()), "GPU RGB pointer");
        if (attr.type != hipMemoryTypeDevice || attr.device != s.device)
            throw std::invalid_argument("RGB must reside on the selected GPU");
    }
    unsigned physical = images.size() == 1 ? 1 : 4;
    std::shared_ptr<Memory> memory;
    for (auto &candidate : s.pool)
        if (candidate.use_count() == 1 && candidate->bytes == physical * elements * sizeof(float)) {
            memory = candidate;
            break;
        }
    if (!memory) {
        memory = std::make_shared<Memory>(s.backend == "npu", physical, s.device);
        s.pool.push_back(memory);
    }
    InputScope audit;
    for (unsigned i = 0; i < physical; i++) {
        const auto &im = images[std::min<size_t>(i, images.size() - 1)];
        int w = im.width, h = im.height, side = std::max(w, h);
        DeviceBuffer tmp(size_t(side) * 256 * 3);
        auto rgb = im.storage->data(), scratch = tmp.data();
        auto dst = static_cast<float *>(memory->device) + i * elements;
        void *horizontal_args[] = {&rgb, &w, &h, &scratch};
        void *vertical_args[] = {&scratch, &side, &dst};
        s.horizontal->launch(side * 256 * 3, horizontal_args);
        s.vertical->launch(elements, vertical_args);
        hip_check(hipDeviceSynchronize(), "MobileCLIP GPU preprocessing completion");
    }
    PreparedInput result;
    result.impl_ = std::make_unique<PreparedInput::Impl>();
    result.impl_->memory = memory;
    result.impl_->owner = &s;
    result.impl_->images = images;
    result.impl_->evidence = {reinterpret_cast<uintptr_t>(memory->host),
                              reinterpret_cast<uintptr_t>(memory->device),
                              0,
                              memory->bytes,
                              0,
                              unsigned(images.size()),
                              physical,
                              ++s.sequence};
    return result;
}
std::vector<std::array<float, 512>> Runtime::infer(PreparedInput &input) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!input.impl_ || input.impl_->owner != &s)
        throw std::invalid_argument("Input belongs to another runtime");
    hip_check(hipSetDevice(s.device), "MobileCLIP inference device");
    auto &e = input.impl_->evidence;
    auto &memory = *input.impl_->memory;
    std::vector<std::array<float, 512>> result(e.physical);
    if (s.backend == "npu") {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape{1, 3, 256, 256};
        auto value = Ort::Value::CreateTensor<float>(mem, static_cast<float *>(memory.host),
                                                     elements, shape.data(), shape.size());
        e.bound = reinterpret_cast<uintptr_t>(value.GetTensorData<float>());
        if (e.bound != e.host)
            throw std::runtime_error("ORT input pointer changed");
        const char *in = "images", *out = "embeddings";
        // The EP call is deliberately outside the application's input-copy scope.
        auto values = s.image->Run(Ort::RunOptions{nullptr}, &in, &value, 1, &out, 1);
        if (values.size() != 1 ||
            values[0].GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, 512} ||
            values[0].GetTensorTypeAndShapeInfo().GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Invalid NPU embedding output");
        std::copy_n(values[0].GetTensorData<float>(), 512, result[0].data());
    } else {
        auto &model = *s.models.at(e.physical);
        auto tensor = model.inputs().front();
        tensor.storage = memory.gpu;
        e.bound = reinterpret_cast<uintptr_t>(tensor.storage->data());
        auto outputs = model.infer({{"images", tensor}});
        readback_metadata(result.data(), outputs[0].storage->data(),
                          e.physical * 512 * sizeof(float));
        e.output_readback_bytes = e.physical * 512 * sizeof(float);
    }
    result.resize(e.valid);
    for (auto &value : result)
        normalize(value);
    return result;
}
std::array<float, 512> Runtime::text(const std::string &text_value) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (text_value.empty() || text_value.size() > 65536 ||
        text_value.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid search text");
    std::array<int64_t, 77> ids{};
    char error[4096]{};
    const char *raw = text_value.c_str();
    if (mc2_tokenizer_encode(s.tokenizer.get(), &raw, 1, ids.data(), error, sizeof(error)))
        throw std::runtime_error(error);
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<int64_t, 2> shape{1, 77};
    auto input =
        Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), shape.size());
    const char *in = "tokens", *out = "embeddings";
    auto values = s.text_session->Run(Ort::RunOptions{nullptr}, &in, &input, 1, &out, 1);
    if (values.size() != 1 ||
        values[0].GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, 512} ||
        values[0].GetTensorTypeAndShapeInfo().GetElementType() !=
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("Invalid text embedding output");
    std::array<float, 512> result;
    std::copy_n(values[0].GetTensorData<float>(), 512, result.data());
    normalize(result);
    return result;
}
} // namespace amdva::mobileclip
