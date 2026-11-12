// Device counting follows the ROCprofiler-SDK device_counting sample API.
// Runs in its own process; no profiler library is loaded into the NVR.
#include "../src/wave_sample.hpp"
#include <hip/hip_runtime.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
volatile sig_atomic_t running = 1;
rocprofiler_context_id_t context{};
rocprofiler_buffer_id_t buffer{};
rocprofiler_counter_config_id_t profile{};
rocprofiler_counter_id_t elapsed_id{}, busy_id{};
size_t expected_busy{};
unsigned render_minor{};
std::string init_error;

void check(rocprofiler_status_t status) {
    if (status != ROCPROFILER_STATUS_SUCCESS)
        throw std::runtime_error(rocprofiler_get_status_string(status));
}
std::string token(const fs::path &path) {
    std::ifstream file(path);
    std::string value;
    file >> value;
    return value;
}
int initialize(rocprofiler_client_finalize_t, void *) {
    try {
        rocprofiler_agent_v0_t chosen{};
        check(rocprofiler_query_available_agents(ROCPROFILER_AGENT_INFO_VERSION_0,
            [](rocprofiler_agent_version_t version, const void **agents, size_t count, void *data) {
                if (version != ROCPROFILER_AGENT_INFO_VERSION_0) return ROCPROFILER_STATUS_ERROR;
                for (size_t i = 0; i < count; ++i) {
                    const auto &a = *static_cast<const rocprofiler_agent_v0_t *>(agents[i]);
                    if (a.type == ROCPROFILER_AGENT_TYPE_GPU && a.drm_render_minor == render_minor)
                        *static_cast<rocprofiler_agent_v0_t *>(data) = a;
                }
                return ROCPROFILER_STATUS_SUCCESS;
            }, sizeof(rocprofiler_agent_t), &chosen));
        if (!chosen.id.handle || chosen.gfx_target_version != 110501)
            throw std::runtime_error("Wave counting is validated only on gfx1151");
        std::vector<rocprofiler_counter_id_t> counters;
        check(rocprofiler_iterate_agent_supported_counters(chosen.id,
            [](rocprofiler_agent_id_t, rocprofiler_counter_id_t *ids, size_t count, void *data) {
                auto &out = *static_cast<std::vector<rocprofiler_counter_id_t> *>(data);
                out.assign(ids, ids + count);
                return ROCPROFILER_STATUS_SUCCESS;
            }, &counters));
        for (auto id : counters) {
            rocprofiler_counter_info_v1_t info{};
            check(rocprofiler_query_counter_info(id, ROCPROFILER_COUNTER_INFO_VERSION_1, &info));
            if (std::string(info.name) == "GRBM_COUNT") {
                if (info.dimensions_instances_count != 1)
                    throw std::runtime_error("Unexpected GRBM_COUNT regions");
                elapsed_id = id;
            }
            if (std::string(info.name) == "SQ_BUSY_CYCLES") {
                busy_id = id;
                expected_busy = info.dimensions_instances_count;
            }
        }
        if (!elapsed_id.handle || !busy_id.handle || !expected_busy || expected_busy > 1024)
            throw std::runtime_error("Required wave counters unavailable");
        rocprofiler_counter_id_t ids[] = {elapsed_id, busy_id};
        check(rocprofiler_create_counter_config(chosen.id, ids, 2, &profile));
        check(rocprofiler_create_context(&context));
        check(rocprofiler_create_buffer(context, 4096, 2048, ROCPROFILER_BUFFER_POLICY_LOSSLESS,
            [](rocprofiler_context_id_t, rocprofiler_buffer_id_t, rocprofiler_record_header_t **,
               size_t, void *, uint64_t) {}, nullptr, &buffer));
        check(rocprofiler_configure_device_counting_service(context, buffer, chosen.id,
            [](rocprofiler_context_id_t ctx, rocprofiler_agent_id_t,
               rocprofiler_device_counting_agent_cb_t set_config, void *) { set_config(ctx, profile); },
            nullptr));
        std::cerr << "wave regions=" << expected_busy << " render_minor=" << render_minor << '\n';
        return 0;
    } catch (const std::exception &error) {
        init_error = error.what();
        return -1;
    }
}
rocprofiler_tool_configure_result_t *configure(uint32_t, const char *, uint32_t,
                                              rocprofiler_client_id_t *id) {
    id->name = "AMDVA wave presence";
    static rocprofiler_tool_configure_result_t result{sizeof(result), initialize, nullptr, nullptr};
    return &result;
}
void pause(unsigned ms) {
    for (unsigned i = 0; running && i < ms; i += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(20u, ms - i)));
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 4) {
            std::cerr << "Usage: amdva-wave-sampler RENDER_NODE SAMPLE_PATH continuous|window\n";
            return 2;
        }
        const fs::path node = argv[1], output = argv[2];
        const std::string mode = argv[3];
        if (mode != "continuous" && mode != "window") throw std::runtime_error("Invalid mode");
        struct stat st{};
        if (stat(node.c_str(), &st) || !S_ISCHR(st.st_mode)) throw std::runtime_error("Invalid render node");
        render_minor = minor(st.st_rdev);
        const auto device = fs::canonical(fs::path("/sys/class/drm") / node.filename() / "device");
        const auto boot = token("/proc/sys/kernel/random/boot_id");
        // Dashboard creates and checks this private directory. Direct invocation is checked too.
        if (lstat(output.parent_path().c_str(), &st) || !S_ISDIR(st.st_mode) ||
            st.st_uid != getuid() || (st.st_mode & 077) != 0)
            throw std::runtime_error("Sample directory must be owned by this user with mode 0700");
        const auto lock_path = output.string() + ".lock";
        const int lock = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
            throw std::runtime_error("Wave collector already active or lock unavailable");
        // Keep lock until exit. A crash is detected by sample expiry by consumers.
        std::signal(SIGTERM, [](int) { running = 0; });
        std::signal(SIGINT, [](int) { running = 0; });
        const auto parent = getppid();
        if (parent <= 1 || prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) return 1;
        check(rocprofiler_force_configure(configure));
        if (!init_error.empty()) throw std::runtime_error(init_error);
        if (hipInit(0) != hipSuccess) throw std::runtime_error("HIP initialization failed");
        // Select by PCI bus ID, never by a fixed HIP ordinal.
        int hip_device{};
        if (hipDeviceGetByPCIBusId(&hip_device, device.filename().c_str()) != hipSuccess ||
            hipSetDevice(hip_device) != hipSuccess)
            throw std::runtime_error("Cannot select HIP device for render node");
        const unsigned epoch = mode == "window" ? 500 : 1000;
        std::vector<rocprofiler_counter_record_t> records(expected_busy + 1);
        std::optional<bool> previously_valid;
        while (running) {
            check(rocprofiler_start_context(context));
            pause(epoch);
            size_t count = records.size();
            const auto status = rocprofiler_sample_device_counting_service(
                context, {}, ROCPROFILER_COUNTER_FLAG_NONE, records.data(), &count);
            const auto stopped = rocprofiler_stop_context(context);
            check(status);
            check(stopped);
            if (!running) break;
            std::vector<double> busy;
            double elapsed = -1;
            unsigned elapsed_count = 0;
            std::set<uint64_t> instances;
            for (size_t i = 0; i < count; ++i) {
                const auto &record = records[i];
                if (!instances.insert(record.id).second) throw std::runtime_error("Duplicate counter region");
                rocprofiler_counter_id_t id{};
                check(rocprofiler_query_record_counter_id(record.id, &id));
                if (id.handle == elapsed_id.handle) { elapsed = record.counter_value; ++elapsed_count; }
                else if (id.handle == busy_id.handle) busy.push_back(record.counter_value);
                else throw std::runtime_error("Unexpected counter");
            }
            if (elapsed_count != 1 || busy.size() != expected_busy)
                throw std::runtime_error("Incomplete counter sample");
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto temporary = output.string() + "." + std::to_string(getpid()) + ".tmp";
            {
                std::ofstream file(temporary);
                file << "AMDVA_WAVE_V1 " << device.filename().string() << ' ' << boot << ' '
                     << now << ' ' << std::setprecision(17) << elapsed << ' ' << busy.size();
                for (auto value : busy) file << ' ' << value;
                file << '\n';
                file.close();
                if (!file) throw std::runtime_error("Cannot write wave sample");
            }
            fs::rename(temporary, output);
            // Invalid (e.g. gated, zero denominator) records are published so consumers fall back immediately.
            const bool valid = amdva::detail::wave_presence(elapsed, busy).has_value();
            if (!previously_valid || *previously_valid != valid) {
                std::cerr << (valid ? "wave counters available\n" : "wave sample unavailable: invalid or gated counters\n");
                previously_valid = valid;
            }
            if (mode == "window") pause(500);
        }
        fs::remove(output);
        close(lock);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "wave collector: " << error.what() << '\n';
        return 1;
    }
}
