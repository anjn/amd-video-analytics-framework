#pragma once
#include <amdva/model.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amdva::mobileclip {
// RGB8 on the selected GPU. Copies of this descriptor retain the allocation.
struct Image {
    unsigned width{}, height{};
    std::shared_ptr<DeviceBuffer> storage;
    int device{};
};
// CPU-image ingestion only; live/video callers supply their existing GPU image.
Image upload(const uint8_t *rgb, unsigned width, unsigned height, int device = 0);
struct Evidence {
    uintptr_t host{}, device{}, bound{};
    size_t input_bytes{}, output_readback_bytes{};
    unsigned valid{}, physical{};
    uint64_t sequence{};
};
class PreparedInput {
  public:
    PreparedInput();
    ~PreparedInput();
    PreparedInput(PreparedInput &&) noexcept;
    PreparedInput &operator=(PreparedInput &&) noexcept;
    PreparedInput(const PreparedInput &) = delete;
    Evidence evidence() const;
    // Diagnostic only: reads normalized input to CPU outside the input handoff.
    std::vector<float> diagnostic_pixels() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Runtime;
};
class Runtime {
  public:
    Runtime(const std::string &bundle, const std::string &backend, int device = 0);
    ~Runtime();
    // Synchronous GPU preprocessing; call under the application's GPU lease.
    PreparedInput prepare(const std::vector<Image> &);
    // NPU inference needs no GPU lease. Input and runtime must outlive this call.
    std::vector<std::array<float, 512>> infer(PreparedInput &);
    std::array<float, 512> text(const std::string &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::mobileclip
