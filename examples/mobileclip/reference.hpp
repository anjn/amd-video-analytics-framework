// CPU reference for validation only, adapted from embeddinggemma2-rocm
// commit 102a9df65cd312529668daabb3e88e0a17cfc363, MIT (third_party/mobileclip-tokenizer/LICENSE).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace reference {
struct ReferenceImage {
    const uint8_t *data;
    size_t width, height, row_stride;
};
inline void require(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
struct Filter {
    int first;
    std::vector<int> weights;
};
std::vector<Filter> filters(int size) {
    std::vector<Filter> result;
    double scale = double(size) / 256, support = std::max(1., scale);
    for (int x = 0; x < 256; x++) {
        double center = (x + .5) * scale;
        int lo = std::max(0, int(center - support + .5)),
            hi = std::min(size, int(center + support + .5));
        Filter f{lo, {}};
        std::vector<double> w;
        double sum = 0;
        for (int k = lo; k < hi; k++) {
            double v = std::max(0., 1 - std::abs((k - center + .5) / support));
            w.push_back(v);
            sum += v;
        }
        for (double v : w)
            f.weights.push_back(int(v / sum * (1 << 22) + .5));
        result.push_back(std::move(f));
    }
    return result;
}
void preprocess(const ReferenceImage &im, float *dst) {
    require(im.data && im.width && im.height, "Image is empty");
    require(im.width <= 16384 && im.height <= 16384, "Image dimensions exceed 16384");
    require(im.row_stride >= im.width * 3 && im.row_stride <= SIZE_MAX / im.height,
            "Invalid RGB row stride");
    int side = int(std::max(im.width, im.height)), ox = (side - int(im.width)) / 2,
        oy = (side - int(im.height)) / 2;
    auto f = filters(side);
    std::vector<uint8_t> tmp(size_t(side) * 256 * 3, 0);
    for (int y = oy; y < oy + int(im.height); y++)
        for (int x = 0; x < 256; x++)
            for (int c = 0; c < 3; c++) {
                int64_t v = 1 << 21;
                for (size_t k = 0; k < f[x].weights.size(); k++) {
                    int sx = f[x].first + int(k) - ox;
                    if (sx >= 0 && sx < int(im.width))
                        v += int64_t(im.data[size_t(y - oy) * im.row_stride + sx * 3 + c]) *
                             f[x].weights[k];
                }
                tmp[(size_t(y) * 256 + x) * 3 + c] = uint8_t(std::clamp<int64_t>(v >> 22, 0, 255));
            }
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            for (int c = 0; c < 3; c++) {
                int64_t v = 1 << 21;
                for (size_t k = 0; k < f[y].weights.size(); k++)
                    v += int64_t(tmp[(size_t(f[y].first + k) * 256 + x) * 3 + c]) * f[y].weights[k];
                dst[(c * 256 + y) * 256 + x] = float(std::clamp<int64_t>(v >> 22, 0, 255)) / 255.f;
            }
}
} // namespace reference
