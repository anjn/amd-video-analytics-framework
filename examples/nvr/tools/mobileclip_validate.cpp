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
        auto wait = [&](auto done) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            while (!done()) {
                if (!controller.fatal().empty())
                    throw std::runtime_error(controller.fatal());
                auto state = search->status();
                if (state.state == "error")
                    throw std::runtime_error(state.error);
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Integration check timeout");
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
            wait([&] { return search->result().request == id; });
            if (!search->result().error.empty())
                throw std::runtime_error(search->result().error);
        };
        query(search->text("a person wearing a blue shirt"));
        query(search->text("青いシャツを着た人"));
        // CPU-origin API input is uploaded once before dispatch to either backend.
        SearchImage cpu{64, 32, std::vector<uint8_t>(64 * 32 * 3, 128)};
        query(search->image(std::move(cpu)));
        controller.ai_runtime()->mobileclip_budget(35, 80);
        if (controller.ai_runtime()->mobileclip_budget() != std::pair<unsigned, unsigned>{35, 80})
            throw std::runtime_error("Budget update failed");
        controller.ai_runtime()->mobileclip_budget(100, 100);
        bool has_children = false;
        for (const auto &task : std::filesystem::directory_iterator("/proc/self/task")) {
            std::ifstream f(task.path() / "children");
            std::string child;
            if (f >> child)
                has_children = true;
        }
        auto stored = search->status().stored;
        unsigned reid_calls = 0, embedding_calls = 0;
        for (const auto &metric : controller.ai_runtime()->snapshot()) {
            if (!metric.camera.empty())
                continue;
            if (metric.stage == "ReID inference + transfer")
                reid_calls += metric.samples;
            if (metric.stage == "Image embedding GPU" || metric.stage == "Image embedding NPU")
                embedding_calls += metric.samples;
        }
        if ((!options.search.reid_model.empty() && !reid_calls) || !embedding_calls)
            throw std::runtime_error("ReID or embedding did not execute");
        controller.shutdown();
        if (has_children || errors() || copies())
            throw std::runtime_error("Child process or copy audit violation");
        std::cout << "{\"schema\":1,\"passed\":true,\"pid\":" << getpid()
                  << ",\"stored\":" << stored << ",\"english_query\":true,\"japanese_query\":true"
                  << ",\"reid_calls\":" << reid_calls << ",\"embedding_calls\":" << embedding_calls
                  << ",\"cpu_image_query\":true,\"budgets_updated\":true,\"child_processes\":false"
                  << ",\"input_copies\":" << copies() << ",\"audit_errors\":" << errors() << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
