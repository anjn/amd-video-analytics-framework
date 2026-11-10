// Standalone diagnostic; build/run instructions: docs/dev/npu-interop.md.
// Optional passthrough checks NPU DMA execution, not model inference zero-copy.
#include <hip/hip_runtime_api.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <xrt/xrt_hw_context.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

static void check(hipError_t rc, const char *op) {
    if (rc != hipSuccess)
        throw std::runtime_error(std::string(op) + ": " + hipGetErrorString(rc));
}
static void fds() {
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        auto target = std::filesystem::read_symlink(entry.path(), ec).string();
        if (target.find("/dev/accel/") == 0)
            std::cout << "accel_fd=" << entry.path().filename() << " " << target << '\n';
    }
}
static void passthrough(xrt::device &device, xrt::bo &input, xrt::bo &output,
                        void *output_gpu, size_t bytes, const char *xclbin, const char *insts) {
    // ABI: MLIR-AIE basic/passthrough_dmas, n=262144 int32.
    xrt::xclbin binary{std::string(xclbin)};
    device.register_xclbin(binary);
    xrt::hw_context context(device, binary.get_uuid());
    auto kernels = binary.get_kernels();
    if (kernels.size() != 1)
        throw std::runtime_error("Expected one passthrough kernel");
    xrt::kernel kernel(context, kernels.front().get_name());
    std::ifstream stream(insts, std::ios::binary | std::ios::ate);
    auto length = stream.tellg();
    if (length <= 0 || length % 4)
        throw std::runtime_error("Invalid instruction binary");
    xrt::bo instructions(device, size_t(length), xrt::bo::flags::cacheable,
                         kernel.group_id(1));
    stream.seekg(0);
    if (!stream.read(instructions.map<char *>(), length))
        throw std::runtime_error("Cannot read instructions");
    instructions.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    xrt::bo unused(device, bytes, xrt::bo::flags::host_only, kernel.group_id(4));
    input.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    auto run = kernel(3u, instructions, size_t(length) / 4, input, unused, output);
    if (run.wait(10000) != ERT_CMD_STATE_COMPLETED)
        throw std::runtime_error("NPU command did not complete");
    output.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    std::vector<unsigned char> observed(bytes);
    check(hipMemcpy(observed.data(), output_gpu, bytes, hipMemcpyDeviceToHost),
          "Diagnostic NPU output readback through HIP");
    if (!std::all_of(observed.begin(), observed.end(), [](unsigned char c) { return c == 0x5a; }))
        throw std::runtime_error("NPU passthrough content mismatch");
    std::cout << "npu_dma_passthrough_verified=1 validation_readback_bytes=" << bytes << '\n';
}
int main(int argc, char **argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
        const std::string order = argc > 1 ? argv[1] : "xrt-first";
        const std::string mode = argc > 2 ? argv[2] : "import";
        if ((argc != 1 && argc != 3 && argc != 5) ||
            (argc == 5 && mode != "register" && mode != "hip-export") ||
            (order != "hip-first" && order != "xrt-first") ||
            (mode != "import" && mode != "register" && mode != "userptr" && mode != "hip-export"))
            throw std::runtime_error("Usage: probe [hip-first|xrt-first] [import|register|userptr|hip-export] [passthrough.xclbin insts.bin]");
        if (order == "hip-first") {
            check(hipInit(0), "hipInit");
            std::cout << "HIP initialized\n";
            fds();
        }
        xrt::device device(0);
        std::cout << "XRT opened\n";
        if (order == "xrt-first")
            check(hipInit(0), "hipInit");
        fds();
        constexpr size_t bytes = 1024 * 1024;
        bool npu_dma_verified = false;
        if (mode == "hip-export") {
            void *gpu = nullptr;
            check(hipMalloc(&gpu, bytes), "hipMalloc");
            int fd = -1;
            check(hipMemGetHandleForAddressRange(&fd, gpu, bytes,
                  hipMemRangeHandleTypeDmaBufFd, 0), "HIP export DMA-BUF");
            try {
                xrt::bo bo(device, fd);
                close(fd);
                fd = -1;
                check(hipMemset(gpu, 0x5a, bytes), "hipMemset");
                check(hipDeviceSynchronize(), "hipDeviceSynchronize");
                if (argc != 5) {
                    auto host = bo.map<unsigned char *>();
                    bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
                    bool equal = std::all_of(host, host + bytes, [](unsigned char c) { return c == 0x5a; });
                    std::cout << "gpu_write_visible_in_xrt_mapping=" << equal << '\n';
                    if (!equal)
                        throw std::runtime_error("Shared allocation content mismatch");
                }
                if (argc == 5) {
                    void *output = nullptr;
                    check(hipMalloc(&output, bytes), "hipMalloc output");
                    int output_fd = -1;
                    try {
                        check(hipMemGetHandleForAddressRange(&output_fd, output, bytes,
                              hipMemRangeHandleTypeDmaBufFd, 0), "HIP export output");
                        xrt::bo result(device, output_fd);
                        close(output_fd);
                        output_fd = -1;
                        check(hipMemset(output, 0, bytes), "Clear output");
                        check(hipDeviceSynchronize(), "Synchronize before NPU");
                        passthrough(device, bo, result, output, bytes, argv[3], argv[4]);
                        npu_dma_verified = true;
                    } catch (...) {
                        if (output_fd >= 0)
                            close(output_fd);
                        (void)hipFree(output);
                        throw;
                    }
                    check(hipFree(output), "hipFree output");
                }
            } catch (...) {
                if (fd >= 0)
                    close(fd);
                (void)hipFree(gpu);
                throw;
            }
            check(hipFree(gpu), "hipFree");
        } else if (mode == "userptr") {
            void *ptr = nullptr;
            check(hipHostMalloc(&ptr, bytes, hipHostMallocMapped), "hipHostMalloc");
            try {
                xrt::bo bo(device, ptr, bytes, xrt::bo::flags::host_only, 0);
                std::cout << "userptr BO created\n";
            } catch (...) {
                (void)hipHostFree(ptr);
                throw;
            }
            check(hipHostFree(ptr), "hipHostFree");
        } else {
            xrt::bo bo(device, bytes, xrt::bo::flags::host_only, 0);
            auto host = bo.map<unsigned char *>();
            void *gpu = nullptr;
            hipExternalMemory_t external = nullptr;
            if (mode == "import") {
                int fd = bo.export_buffer();
                std::cout << "export_fd=" << fd << '\n';
                hipExternalMemoryHandleDesc desc{};
                desc.type = hipExternalMemoryHandleTypeOpaqueFd;
                desc.handle.fd = fd;
                desc.size = bytes;
                auto rc = hipImportExternalMemory(&external, &desc);
                // HIP does not take ownership of the supplied FD.
                close(fd);
                check(rc, "hipImportExternalMemory");
                hipExternalMemoryBufferDesc buffer{};
                buffer.size = bytes;
                check(hipExternalMemoryGetMappedBuffer(&gpu, external, &buffer), "HIP map");
            } else {
                check(hipHostRegister(host, bytes, hipHostRegisterMapped), "hipHostRegister");
                check(hipHostGetDevicePointer(&gpu, host, 0), "hipHostGetDevicePointer");
            }
            check(hipMemset(gpu, 0x5a, bytes), "hipMemset");
            check(hipDeviceSynchronize(), "hipDeviceSynchronize");
            if (argc == 5) {
                xrt::bo result(device, bytes, xrt::bo::flags::host_only, 0);
                auto out_host = result.map<void *>();
                check(hipHostRegister(out_host, bytes, hipHostRegisterMapped), "Register output");
                void *out_gpu = nullptr;
                check(hipHostGetDevicePointer(&out_gpu, out_host, 0), "Map output");
                try {
                    check(hipMemset(out_gpu, 0, bytes), "Clear output");
                    check(hipDeviceSynchronize(), "Synchronize before NPU");
                    passthrough(device, bo, result, out_gpu, bytes, argv[3], argv[4]);
                    npu_dma_verified = true;
                } catch (...) {
                    (void)hipHostUnregister(out_host);
                    throw;
                }
                check(hipHostUnregister(out_host), "Unregister output");
            }
            bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            bool equal = std::all_of(host, host + bytes, [](unsigned char c) { return c == 0x5a; });
            std::cout << "gpu_write_visible_in_xrt_mapping=" << equal << '\n';
            if (mode == "import") {
                check(hipFree(gpu), "hipFree");
                check(hipDestroyExternalMemory(external), "hipDestroyExternalMemory");
            } else
                check(hipHostUnregister(host), "hipHostUnregister");
            if (!equal)
                throw std::runtime_error("Shared allocation content mismatch");
        }
        std::cout << "allocation_probe_passed=1 npu_dma_verified=" << npu_dma_verified
                  << " model_zero_copy_verified=0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "probe: " << e.what() << '\n';
        return 1;
    }
}
