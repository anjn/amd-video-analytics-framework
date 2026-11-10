// Test-only native embedding ABI. Fail either the first warmup or the first live image call.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
struct Context {
    unsigned calls{};
    bool warmup{};
};
extern "C" int eg2_create(const char *bundle, int, void **out, char *, size_t) {
    *out = new Context{0, std::string(bundle).find("warmup") != std::string::npos};
    return 0;
}
extern "C" void eg2_destroy(void *ptr) { delete static_cast<Context *>(ptr); }
extern "C" int eg2_encode_images(void *ptr, const uint8_t *, size_t, size_t, size_t, int,
                                 float *out, size_t count, char *error, size_t bytes) {
    auto &c = *static_cast<Context *>(ptr);
    if (++c.calls > (c.warmup ? 0u : 2u)) {
        std::snprintf(error, bytes, "Simulated embedding %s failure",
                      c.warmup ? "warmup" : "runtime");
        return 1;
    }
    std::fill_n(out, count, 0.f);
    for (size_t i = 0; i < count; i += 768)
        out[i] = 1;
    return 0;
}
extern "C" int eg2_encode_texts(void *, const char *const *, size_t, const char *, float *out,
                                size_t count, char *, size_t) {
    std::fill_n(out, count, 0.f);
    out[0] = 1;
    return 0;
}
