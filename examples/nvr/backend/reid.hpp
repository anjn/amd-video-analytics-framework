#pragma once
#include "backend/person_tracking.hpp"
#include "backend/search.hpp"
namespace amdva::nvr {
struct ReIdInput {
    std::shared_ptr<GstSample> frame;
    Detection box;
};
class ReIdModel {
    struct Preprocessor;
    ModelExecutor model_;
    DeviceTensor input_;
    std::unique_ptr<Preprocessor> preprocessor_;

  public:
    explicit ReIdModel(const std::string &path, unsigned device);
    ~ReIdModel();
    ReIdFeature encode(const ReIdInput &);
    std::vector<ReIdFeature> encode_many(const std::vector<ReIdInput> &);
    double prepare_ms{}, inference_ms{};
    const std::vector<size_t> &input_shape() const { return input_.shape; }
};
} // namespace amdva::nvr
