#include "reference.hpp"
#include <amdva/mobileclip.hpp>
#include <chrono>
#include <dlfcn.h>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <unistd.h>
using namespace amdva;
using namespace amdva::mobileclip;
namespace {
struct Reference {
    void *lib{}, *ctx{};
    void (*destroy)(void *){};
    int (*encode)(void *, const reference::ReferenceImage *, size_t, float *, size_t, char *,
                  size_t){};
    int (*encode_text)(void *, const char *const *, size_t, float *, size_t, char *, size_t){};
    Reference(const std::string &path, const std::string &bundle, const std::string &backend) {
        if (path.empty())
            return;
        lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            throw std::runtime_error(dlerror());
        auto create =
            reinterpret_cast<int (*)(const char *, const char *, void **, char *, size_t)>(
                dlsym(lib, "mc2_create"));
        destroy = reinterpret_cast<void (*)(void *)>(dlsym(lib, "mc2_destroy"));
        encode = reinterpret_cast<decltype(encode)>(dlsym(lib, "mc2_encode_images"));
        encode_text = reinterpret_cast<decltype(encode_text)>(dlsym(lib, "mc2_encode_texts"));
        if (!create || !destroy || !encode || !encode_text)
            throw std::runtime_error("Missing reference C ABI");
        char error[4096]{};
        if (create(bundle.c_str(), backend.c_str(), &ctx, error, sizeof(error)))
            throw std::runtime_error(error);
    }
    ~Reference() {
        if (ctx)
            destroy(ctx);
        if (lib)
            dlclose(lib);
    }
};
double cosine(const std::array<float, 512> &a, const float *b) {
    double dot = 0, aa = 0, bb = 0;
    for (unsigned i = 0; i < 512; i++) {
        dot += a[i] * double(b[i]);
        aa += a[i] * double(a[i]);
        bb += b[i] * double(b[i]);
    }
    return dot / std::sqrt(aa * bb);
}
template <class F> F symbol(const char *name) {
    return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
}
} // namespace
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: amdva-mobileclip-interop BUNDLE [gpu|npu|gpu+npu] [iterations>=100] "
                     "[reference-libmobileclip2.so]\n";
        return 2;
    }
    try {
        std::string bundle = argv[1], mode = argc > 2 ? argv[2] : "gpu+npu",
                    reference_lib = argc > 4 ? argv[4] : "";
        unsigned iterations = argc > 3 ? std::stoul(argv[3]) : 100;
        if (iterations < 100 || (mode != "gpu" && mode != "npu" && mode != "gpu+npu"))
            throw std::invalid_argument("Invalid mode or iterations");
        auto start = symbol<void (*)()>("amdva_audit_start");
        auto stop = symbol<void (*)()>("amdva_audit_stop");
        auto errors = symbol<unsigned long long (*)()>("amdva_audit_errors");
        auto input_copies = symbol<unsigned long long (*)()>("amdva_audit_mobileclip_input_copies");
        if (!start || !stop || !errors || !input_copies)
            throw std::runtime_error("Run with LD_PRELOAD=build/src/gst/libamdva_copy_audit.so");
        hip_check(hipSetDevice(0), "Sample GPU");
        Kernel fill(R"(
extern "C" __global__ void fill(unsigned char *p,unsigned bytes,unsigned seed) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i<bytes)p[i]=(i*37+seed*53+(i/17)*11)%256;
})",
                    "fill", gpu_arch(0));
        std::unique_ptr<Runtime> npu, gpu;
        if (mode != "gpu")
            npu = std::make_unique<Runtime>(bundle, "npu");
        if (mode != "npu")
            gpu = std::make_unique<Runtime>(bundle, "gpu");
        std::unique_ptr<Reference> npu_ref, gpu_ref;
        if (npu)
            npu_ref = std::make_unique<Reference>(reference_lib, bundle, "npu");
        if (gpu)
            gpu_ref = std::make_unique<Reference>(reference_lib, bundle, "gpu");
        start();
        auto before_errors = errors(), before_copies = input_copies();
        double min_cosine = 1, max_pixel_error = 0, total_ms = 0;
        double reference_ms = 0;
        unsigned reference_calls = 0;
        uint64_t diagnostic_bytes = 0;
        unsigned calls = 0;
        uintptr_t previous_gpu = 0, previous_npu = 0;
        unsigned reuse_count = 0, rejected = 0;
        std::mutex measurements;
        auto exemption = symbol<void (*)(bool)>("amdva_audit_embedding_scope");
        for (unsigned run = 0; run < iterations; run++) {
            const std::pair<unsigned, unsigned> sizes[] = {
                {108, 192}, {513, 293}, {32, 19}, {256, 256}, {1, 1}};
            auto [w, h] = sizes[run % 5];
            unsigned seed = run % 2, bytes = w * h * 3;
            Image im{w, h, std::make_shared<DeviceBuffer>(bytes), 0};
            auto ptr = im.storage->data();
            void *args[] = {&ptr, &bytes, &seed};
            fill.launch(bytes, args);
            hip_check(hipDeviceSynchronize(), "Generated image ready");
            if (run == 0) {
                auto invalid = [&](auto action) {
                    try {
                        action();
                    } catch (const std::invalid_argument &) {
                        ++rejected;
                        return;
                    }
                    throw std::runtime_error("Invalid input was accepted");
                };
                for (auto *runtime : {gpu.get(), npu.get()})
                    if (runtime) {
                        invalid([&] { runtime->prepare({}); });
                        auto malformed = im;
                        malformed.width = 0;
                        invalid([&] { runtime->prepare({malformed}); });
                        malformed = im;
                        malformed.device = 1;
                        invalid([&] { runtime->prepare({malformed}); });
                        invalid([&] { runtime->text(""); });
                    }
                if (gpu && npu) {
                    auto foreign = gpu->prepare({im});
                    invalid([&] { npu->infer(foreign); });
                }
            }
            std::vector<uint8_t> cpu(bytes);
            for (unsigned i = 0; i < bytes; i++)
                cpu[i] = (i * 37 + seed * 53 + (i / 17) * 11) % 256;
            reference::ReferenceImage ref_im{cpu.data(), w, h, size_t(w) * 3};
            std::vector<float> expected(3 * 256 * 256);
            reference::preprocess(ref_im, expected.data());
            auto test = [&](Runtime &runtime, Reference &ref, bool on_npu) {
                const unsigned valid = on_npu ? 1u : unsigned(run % 4 + 1);
                std::vector<Image> images(valid, im);
                auto t = std::chrono::steady_clock::now();
                auto input = runtime.prepare(images);
                auto result = runtime.infer(input);
                auto e = input.evidence();
                double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
                        .count();
                std::lock_guard<std::mutex> measurement_lock(measurements);
                // Reference and diagnostic readbacks are measured outside input handoff.
                exemption(true);
                auto observed = input.diagnostic_pixels();
                diagnostic_bytes += e.input_bytes;
                for (size_t i = 0; i < observed.size(); i++)
                    max_pixel_error =
                        std::max(max_pixel_error,
                                 double(std::abs(observed[i] - expected[i % expected.size()])));
                if (on_npu ? (e.host == 0 || e.bound != e.host)
                           : (e.host != 0 || e.bound != e.device))
                    throw std::runtime_error("Input alias proof failed");
                auto &previous = on_npu ? previous_npu : previous_gpu;
                if (previous == e.device)
                    ++reuse_count;
                previous = e.device;
                if (ref.ctx) {
                    unsigned physical = on_npu ? 1u : (valid == 1 ? 1u : 4u);
                    std::vector<reference::ReferenceImage> refs(physical, ref_im);
                    std::vector<float> baseline(physical * 512);
                    char error[4096]{};
                    auto reference_start = std::chrono::steady_clock::now();
                    if (ref.encode(ref.ctx, refs.data(), physical, baseline.data(), baseline.size(),
                                   error, sizeof(error)))
                        throw std::runtime_error(error);
                    reference_ms += std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - reference_start)
                                        .count();
                    ++reference_calls;
                    for (unsigned i = 0; i < valid; i++) {
                        double c = cosine(result[i], baseline.data() + i * 512);
                        if (!std::isfinite(c) || c < .9999)
                            throw std::runtime_error("Embedding parity failed");
                        min_cosine = std::min(min_cosine, c);
                    }
                }
                exemption(false);
                return std::make_pair(e, ms);
            };
            std::future<std::pair<Evidence, double>> pending;
            if (npu)
                pending =
                    std::async(std::launch::async, [&] { return test(*npu, *npu_ref, true); });
            if (gpu) {
                auto x = test(*gpu, *gpu_ref, false);
                total_ms += x.second;
                ++calls;
                if (run == 0)
                    std::cout << "{\"backend\":\"gpu\",\"pid\":" << getpid()
                              << ",\"device\":" << x.first.device << ",\"bound\":" << x.first.bound
                              << "}\n";
            }
            if (pending.valid()) {
                auto x = pending.get();
                total_ms += x.second;
                ++calls;
                if (run == 0)
                    std::cout << "{\"backend\":\"npu\",\"pid\":" << getpid()
                              << ",\"host\":" << x.first.host << ",\"device\":" << x.first.device
                              << ",\"ort_bound\":" << x.first.bound << "}\n";
            }
        }
        auto english = npu ? npu->text("a person wearing a blue shirt")
                           : gpu->text("a person wearing a blue shirt");
        auto japanese = npu ? npu->text("青いシャツを着た人") : gpu->text("青いシャツを着た人");
        if (!std::isfinite(english[0]) || !std::isfinite(japanese[0]))
            throw std::runtime_error("Invalid text result");
        double min_text_cosine = 1;
        auto &text_reference = npu ? *npu_ref : *gpu_ref;
        if (text_reference.ctx) {
            const char *texts[] = {"a person wearing a blue shirt", "青いシャツを着た人"};
            std::array<float, 1024> baseline{};
            char error[4096]{};
            if (text_reference.encode_text(text_reference.ctx, texts, 2, baseline.data(),
                                           baseline.size(), error, sizeof(error)))
                throw std::runtime_error(error);
            min_text_cosine =
                std::min(cosine(english, baseline.data()), cosine(japanese, baseline.data() + 512));
            if (!std::isfinite(min_text_cosine) || min_text_cosine < .9999)
                throw std::runtime_error("Text embedding parity failed");
        }
        auto copies = input_copies() - before_copies, violations = errors() - before_errors;
        stop();
        bool pass = copies == 0 && violations == 0 && max_pixel_error == 0 && reuse_count > 0 &&
                    rejected > 0;
        std::cout << std::setprecision(12) << "{\"schema\":1,\"application_input_copy_verified\":"
                  << (pass ? "true" : "false")
                  << ",\"ep_internal_copy_verified\":false,\"iterations\":" << iterations
                  << ",\"calls\":" << calls << ",\"input_copies\":" << copies
                  << ",\"violations\":" << violations << ",\"max_pixel_error\":" << max_pixel_error
                  << ",\"reference_checked\":" << (!reference_lib.empty() ? "true" : "false")
                  << ",\"min_embedding_cosine\":" << min_cosine
                  << ",\"min_text_cosine\":" << min_text_cosine
                  << ",\"buffer_reuses\":" << reuse_count
                  << ",\"invalid_inputs_rejected\":" << rejected
                  << ",\"diagnostic_input_readback_bytes\":" << diagnostic_bytes
                  << ",\"reference_mean_call_ms\":"
                  << (reference_calls ? reference_ms / reference_calls : 0)
                  << ",\"mean_call_ms\":" << total_ms / calls << "}\n";
        return pass ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "MobileCLIP sample: " << e.what() << '\n';
        return 1;
    }
}
