// Fixed-load cadence and responsiveness check. Run with the copy auditor preloaded.
#include "backend/config.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: amdva-scheduling-validate CONFIG [SECONDS=120]\n";
        return 2;
    }
    try {
        initialize_runtime();
        auto options = load_config(argv[1]);
        const unsigned seconds = argc == 3 ? std::stoul(argv[2]) : 120;
        if (!seconds)
            throw std::runtime_error("Empty measurement");
        options.preview_enabled = false;
        Controller controller(options);
        auto runtime = controller.ai_runtime();
        auto *search = controller.search_service();
        if (!search)
            throw std::runtime_error("Search required");
        auto check = [&] {
            if (!controller.fatal().empty())
                throw std::runtime_error(controller.fatal());
            auto s = search->status();
            if (!s.tracking_error.empty())
                throw std::runtime_error(s.tracking_error);
            if (!s.error.empty() || s.state == "error")
                throw std::runtime_error(s.error);
        };
        auto sleep = [&](unsigned ms) {
            for (unsigned i = 0; i < ms; i += 100) {
                check();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        };
        const auto startup_deadline = Clock::now() + std::chrono::seconds(180);
        while (!controller.status_ready() || search->status().state != "ready") {
            if (Clock::now() > startup_deadline)
                throw std::runtime_error("Startup timed out");
            sleep(100);
        }
        sleep(15000);
        auto start_audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto copies = reinterpret_cast<unsigned long long (*)()>(
            dlsym(RTLD_DEFAULT, "amdva_audit_mobileclip_input_copies"));
        if (!start_audit || !errors || !copies)
            throw std::runtime_error("Preload copy auditor");
        start_audit();
        const auto initial = search->status();
        runtime->reset_measurements();
        const auto started = Clock::now();
        std::map<std::string, AiMetric> peaks;
        for (unsigned i = 0; i < seconds; ++i) {
            sleep(1000);
            for (const auto &m : runtime->snapshot())
                if (m.camera.empty() && m.samples) {
                    auto &p = peaks[m.stage];
                    p.p95_ms = std::max(p.p95_ms, m.p95_ms);
                    p.max_ms = std::max(p.max_ms, m.max_ms);
                }
        }
        const double duration = std::chrono::duration<double>(Clock::now() - started).count();
        auto metrics = runtime->snapshot();
        auto state = search->status();
        auto count = [&](const std::string &stage, const std::string &camera) {
            for (const auto &m : metrics)
                if (m.stage == stage && m.camera == camera)
                    return m.completed;
            return uint64_t{};
        };
        bool detection = true;
        // Provider diagnostics may use stdout. Publish our complete JSON only after shutdown.
        std::ostringstream report;
        report << "{\"schema\":1,\"seconds\":" << duration
               << ",\"warmup_seconds\":15,\"configured_fps\":" << options.ai_fps
               << ",\"interval_ms\":" << options.search.interval_ms << ",\"cameras\":[";
        bool comma = false;
        for (const auto &source : options.sources) {
            const bool continuous = source.selected || !options.search.reid_model.empty();
            if (!continuous)
                continue;
            const double fps = count("Detection update", source.id) / duration;
            detection &= fps >= options.ai_fps * .9;
            if (comma)
                report << ',';
            comma = true;
            report << "{\"id\":\"" << source.id << "\",\"detection_fps\":" << fps << '}';
        }
        const auto eligible = count("Tracking eligible", ""),
                   tracked = count("Tracking association", "");
        const bool tracking = eligible && double(tracked) / eligible >= .9;
        const bool npu = options.search.embedding_model != "mobileclip2-s2" ||
                         options.search.mobileclip_backend == "gpu" ||
                         (count("Image embedding NPU total", "") &&
                          peaks["Image embedding NPU total"].p95_ms <= 50);
        const auto first_misses = state.first_deadline_misses - initial.first_deadline_misses;
        const auto update_misses = state.update_deadline_misses - initial.update_deadline_misses;
        const bool first = !first_misses && peaks["Embedding first result"].max_ms <= 1000;
        const bool updates = !update_misses && peaks["Embedding update interval"].p95_ms <=
                                                   options.search.interval_ms * 1.2;
        const bool functional =
            state.stored > initial.stored && count("ReID inference + transfer", "");
        controller.shutdown();
        const bool shutdown_ok = controller.fatal().empty() && search->status().error.empty();
        const bool passed = shutdown_ok && detection && tracking && npu && first && updates &&
                            functional && !errors() && !copies();
        report << "],\"tracking_eligible\":" << eligible << ",\"tracking_completed\":" << tracked
               << ",\"first_deadline_misses\":" << first_misses
               << ",\"update_deadline_misses\":" << update_misses
               << ",\"stored\":" << (state.stored - initial.stored) << ",\"stages\":[";
        comma = false;
        for (const auto &m : metrics)
            if (m.camera.empty()) {
                if (comma)
                    report << ',';
                comma = true;
                report << "{\"stage\":\"" << m.stage << "\",\"completed\":" << m.completed
                       << ",\"mean_ms\":" << m.mean_ms
                       << ",\"worst_window_p95_ms\":" << peaks[m.stage].p95_ms
                       << ",\"max_ms\":" << peaks[m.stage].max_ms << ",\"dropped\":" << m.dropped
                       << '}';
            }
        report << "],\"detection_pass\":" << (detection ? "true" : "false")
               << ",\"tracking_pass\":" << (tracking ? "true" : "false")
               << ",\"npu_latency_pass\":" << (npu ? "true" : "false")
               << ",\"first_embedding_pass\":" << (first ? "true" : "false")
               << ",\"embedding_interval_pass\":" << (updates ? "true" : "false")
               << ",\"shutdown_pass\":" << (shutdown_ok ? "true" : "false")
               << ",\"audit_errors\":" << errors() << ",\"input_copies\":" << copies()
               << ",\"passed\":" << (passed ? "true" : "false") << "}\n";
        std::cout << report.str();
        return passed ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
