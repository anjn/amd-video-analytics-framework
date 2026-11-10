#include "amdva/npu_meta.hpp"
#include <iostream>
#include <limits>
using namespace amdva;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    try {
        gst_init(&argc, &argv);
        for (auto pair : {std::pair{TensorType::Float32, 4u},
                          {TensorType::Float16, 2u},
                          {TensorType::Int32, 4u},
                          {TensorType::Int64, 8u},
                          {TensorType::UInt8, 1u}})
            check(TensorDescriptor{"x", pair.first, {2, 3}}.bytes() == 6 * pair.second,
                  "dtype size");
        for (auto shape : {std::vector<size_t>{0}, {std::numeric_limits<size_t>::max(), 2}}) {
            bool rejected = false;
            try {
                TensorDescriptor{"x", TensorType::Float32, shape}.bytes();
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            check(rejected, "invalid shape accepted");
        }
        auto values =
            std::make_shared<std::vector<int64_t>>(std::initializer_list<int64_t>{17, 42});
        std::weak_ptr<std::vector<int64_t>> weak = values;
        auto result = std::make_shared<NpuResult>();
        result->tensors.push_back({{"ids", TensorType::Int64, {2}}, values->data(), values});
        auto *buffer = gst_buffer_new_allocate(nullptr, 8, nullptr);
        GST_BUFFER_PTS(buffer) = 7 * GST_SECOND;
        check(add_npu_result_meta(buffer, result), "attach output");
        check(!add_npu_result_meta(buffer, result), "duplicate output accepted");
        check(!add_shared_tensor_meta(buffer, nullptr), "null input accepted");
        auto *copy = gst_buffer_copy(buffer);
        auto *deep = gst_buffer_copy_deep(buffer);
        auto *region = gst_buffer_copy_region(buffer, GST_BUFFER_COPY_ALL, 1, 2);
        check(get_npu_result_meta(copy) && get_npu_result_meta(deep), "full copy lost output");
        check(!get_npu_result_meta(region), "region retained whole result");
        check(gst_buffer_peek_memory(buffer, 0) == gst_buffer_peek_memory(copy, 0),
              "payload copied");
        check(GST_BUFFER_PTS(copy) == GST_BUFFER_PTS(buffer), "PTS changed");
        check((*get_npu_result_meta(deep)->result)->tensors[0].data == values->data(),
              "raw tensor copied");
        result.reset();
        values.reset();
        gst_buffer_unref(buffer);
        gst_buffer_unref(region);
        gst_buffer_unref(copy);
        check(!weak.expired(), "output freed before deep copy");
        check(static_cast<const int64_t *>(
                  (*get_npu_result_meta(deep)->result)->tensors[0].data)[1] == 42,
              "retained output corrupted");
        gst_buffer_unref(deep);
        check(weak.expired(), "output owner leaked");
        std::cout << "NPU descriptor and raw output metadata ownership passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
