// Headless NVR integration check; fixtures are served by amdva-rtsp-files.
#include "backend/config.hpp"
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: amdva-mobileclip-nvr-validate CONFIG\n";
        return 2;
    }
    try {
        initialize_runtime();
        auto options = load_config(argv[1]);
        if (options.search.embedding_model != "mobileclip2-s2")
            throw std::runtime_error("Select MobileCLIP");
        options.preview_enabled = false;
        Controller controller(options);
        auto *search = controller.search_service();
        if (!search)
            throw std::runtime_error("Search disabled");
        std::string phase = "collection";
        auto wait = [&](auto done) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            while (!done()) {
                if (!controller.fatal().empty())
                    throw std::runtime_error(controller.fatal());
                auto state = search->status();
                if (!state.tracking_error.empty())
                    throw std::runtime_error(state.tracking_error);
                if (state.state == "error" || !state.error.empty())
                    throw std::runtime_error(state.error);
                if (std::chrono::steady_clock::now() > deadline) {
                    for (const auto &m : controller.ai_runtime()->snapshot())
                        if (m.camera.empty())
                            std::cerr << m.stage << " samples=" << m.samples
                                      << " mean_ms=" << m.mean_ms << " wait_ms=" << m.wait_ms
                                      << " pending=" << m.pending << '\n';
                    throw std::runtime_error("Integration check timeout in " + phase +
                                             " stored=" + std::to_string(state.stored) +
                                             " pending=" + std::to_string(state.pending));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
        wait([&] { return search->status().stored >= 6; });
        auto start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto copies = reinterpret_cast<unsigned long long (*)()>(
            dlsym(RTLD_DEFAULT, "amdva_audit_mobileclip_input_copies"));
        if (!start || !errors || !copies)
            throw std::runtime_error("Load copy auditor with LD_PRELOAD");
        start();
        auto query = [&](uint64_t id) {
            phase = "query " + std::to_string(id);
            wait([&] { return search->result().request == id; });
            if (!search->result().error.empty())
                throw std::runtime_error(search->result().error);
        };
        query(search->text("a person wearing a blue shirt"));
        query(search->text("青いシャツを着た人"));
        // CPU-origin API input is uploaded once before dispatch to either backend.
        SearchImage cpu{64, 32, std::vector<uint8_t>(64 * 32 * 3, 128)};
        query(search->image(std::move(cpu)));
        controller.ai_runtime()->configure(3.5, 700);
        if (controller.ai_runtime()->fps() != 3.5 || controller.ai_runtime()->interval_ms() != 700)
            throw std::runtime_error("Frequency update failed");
        controller.ai_runtime()->configure(options.ai_fps, options.search.interval_ms);
        bool has_children = false;
        for (const auto &task : std::filesystem::directory_iterator("/proc/self/task")) {
            std::ifstream f(task.path() / "children");
            std::string child;
            if (f >> child)
                has_children = true;
        }
        auto stored = search->status().stored;
        unsigned reid_calls = 0, embedding_calls = 0;
        double npu_ms = 0, npu_wait_ms = 0, npu_prepare_ms = 0, npu_total_ms = 0;
        for (const auto &metric : controller.ai_runtime()->snapshot()) {
            if (!metric.camera.empty())
                continue;
            if (metric.stage == "Image embedding NPU")
                npu_ms = metric.mean_ms;
            if (metric.stage == "Image embedding NPU preprocess") {
                npu_wait_ms = metric.wait_ms;
                npu_prepare_ms = metric.mean_ms;
            }
            if (metric.stage == "Image embedding NPU total")
                npu_total_ms = metric.mean_ms;
            if (metric.stage == "ReID inference + transfer")
                reid_calls += metric.samples;
            if (metric.stage == "Image embedding GPU" || metric.stage == "Image embedding NPU")
                embedding_calls += metric.samples;
        }
        if ((!options.search.reid_model.empty() && !reid_calls) || !embedding_calls)
            throw std::runtime_error("ReID or embedding did not execute");
        controller.shutdown();
        if (!search->status().error.empty())
            throw std::runtime_error(search->status().error);
        if (has_children || errors() || copies())
            throw std::runtime_error("Child process or copy audit violation");
        std::cout
            << "{\"schema\":1,\"passed\":true,\"pid\":" << getpid() << ",\"stored\":" << stored
            << ",\"english_query\":true,\"japanese_query\":true" << ",\"reid_calls\":" << reid_calls
            << ",\"embedding_calls\":" << embedding_calls << ",\"npu_inference_ms\":" << npu_ms
            << ",\"npu_gpu_wait_ms\":" << npu_wait_ms
            << ",\"npu_gpu_prepare_ms\":" << npu_prepare_ms << ",\"npu_total_ms\":" << npu_total_ms
            << ",\"cpu_image_query\":true,\"frequencies_updated\":true,\"child_processes\":false"
            << ",\"input_copies\":" << copies() << ",\"audit_errors\":" << errors() << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
