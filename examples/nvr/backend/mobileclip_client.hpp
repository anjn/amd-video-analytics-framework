#pragma once
#include "backend/search.hpp"
#include <algorithm>
#ifdef AMDVA_HAS_MOBILECLIP
#include <amdva/mobileclip.hpp>
#endif
namespace amdva::nvr {
// In-process adapter. The legacy library/worker/venv config fields are ignored.
class MobileClipClient {
#ifdef AMDVA_HAS_MOBILECLIP
    mobileclip::Runtime runtime_;
    int device_;

  public:
    explicit MobileClipClient(const SearchOptions &o)
        : runtime_(o.mobileclip_bundle, o.mobileclip_backend, int(o.device)),
          device_(int(o.device)) {}
    void ingest(SearchImage &image) {
        if (!image.gpu_rgb) {
            if (image.rgb.size() != size_t(image.width) * image.height * 3)
                throw std::invalid_argument("Invalid CPU RGB image");
            auto gpu = mobileclip::upload(image.rgb.data(), image.width, image.height, device_);
            image.gpu_rgb = std::move(gpu.storage);
            image.gpu_device = device_;
        }
    }
    mobileclip::PreparedInput prepare(const std::vector<SearchImage> &images) {
        std::vector<mobileclip::Image> input;
        for (const auto &im : images) {
            if (im.gpu_rgb)
                input.push_back({im.width, im.height, im.gpu_rgb, im.gpu_device});
            else {
                if (im.rgb.size() != size_t(im.width) * im.height * 3)
                    throw std::invalid_argument("Invalid CPU RGB image");
                input.push_back(mobileclip::upload(im.rgb.data(), im.width, im.height, device_));
            }
        }
        return runtime_.prepare(input);
    }
    std::vector<std::array<float, 768>> infer(mobileclip::PreparedInput &input) {
        auto values = runtime_.infer(input);
        std::vector<std::array<float, 768>> out(values.size());
        for (size_t i = 0; i < values.size(); i++)
            std::copy(values[i].begin(), values[i].end(), out[i].begin());
        return out;
    }
    std::vector<std::array<float, 768>> images(const std::vector<SearchImage> &images) {
        auto input = prepare(images);
        return infer(input);
    }
    std::array<float, 768> text(const std::string &value) {
        auto embedding = runtime_.text(value);
        std::array<float, 768> out{};
        std::copy(embedding.begin(), embedding.end(), out.begin());
        return out;
    }
#else
  public:
    explicit MobileClipClient(const SearchOptions &) {
        throw std::runtime_error("MobileCLIP requires a build with AMDVA_BUILD_MOBILECLIP=ON and "
                                 "the Ryzen AI runtime environment");
    }
    std::vector<std::array<float, 768>> images(const std::vector<SearchImage> &) {
        throw std::runtime_error("MobileCLIP unavailable");
    }
    std::array<float, 768> text(const std::string &) {
        throw std::runtime_error("MobileCLIP unavailable");
    }
#endif
};
} // namespace amdva::nvr
