#pragma once
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace amdva::dashboard {
// One optional helper per render node. ResourceMonitor itself stays synchronous.
class WaveCollector {
    pid_t child_ = -1;
    std::string sample_;
public:
    explicit WaveCollector(const std::string &node) {
        try {
            // GRBM normalization is not a validated wall-time fraction under DVFS.
            // Keep sysfs as the default; explicit modes expose the diagnostic candidate.
            const char *requested = std::getenv("AMDVA_GPU_WAVE_MODE");
            const std::string mode = requested ? requested : "sysfs";
            if (mode == "sysfs") return;
            if (mode != "continuous" && mode != "window")
                throw std::runtime_error("AMDVA_GPU_WAVE_MODE must be sysfs, continuous or window");
            std::ifstream os("/etc/os-release");
            std::string line;
            bool supported_os = false, ubuntu = false;
            while (std::getline(os, line)) {
                if (line == "VERSION_ID=\"26.04\"" || line == "VERSION_ID=26.04") supported_os = true;
                if (line == "ID=ubuntu" || line == "ID=\"ubuntu\"") ubuntu = true;
            }
            if (!supported_os || !ubuntu) throw std::runtime_error("Wave counting is validated only on Ubuntu 26.04");
            namespace fs = std::filesystem;
            const auto runtime = fs::path("/run/user") / std::to_string(getuid());
            struct stat st{};
            if (lstat(runtime.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077))
                throw std::runtime_error("Private user runtime directory unavailable");
            const auto directory = runtime / "amdva-wave";
            if (mkdir(directory.c_str(), 0700) && errno != EEXIST)
                throw std::runtime_error("Cannot create wave runtime directory");
            if (lstat(directory.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077))
                throw std::runtime_error("Unsafe wave runtime directory");
            const auto pci = fs::canonical(fs::path("/sys/class/drm") / fs::path(node).filename() / "device").filename();
            sample_ = (directory / (pci.string() + ".sample")).string();
            auto executable = fs::canonical("/proc/self/exe").parent_path() / "amdva-wave-sampler";
            // In the build tree the helper lives at the top level; installed binaries are siblings.
            if (!fs::exists(executable)) executable = fs::canonical("/proc/self/exe").parent_path().parent_path().parent_path() / "amdva-wave-sampler";
            if (!fs::exists(executable)) throw std::runtime_error("amdva-wave-sampler is not installed");
            std::string program = executable.string();
            char *args[] = {program.data(), const_cast<char *>(node.c_str()), sample_.data(), const_cast<char *>(mode.c_str()), nullptr};
            // Never inherit copy auditing or another profiler into this isolated process.
            std::vector<std::string> environment;
            for (char **p = environ; *p; ++p) {
                const std::string value = *p;
                if (value.rfind("LD_PRELOAD=", 0) && value.rfind("ROCP_TOOL_LIBRARIES=", 0)) environment.push_back(value);
            }
            std::vector<char *> env;
            for (auto &value : environment) env.push_back(value.data());
            env.push_back(nullptr);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            const auto log = (directory / (pci.string() + ".log")).string();
            int error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
            if (!error) error = posix_spawn(&child_, program.c_str(), &actions, nullptr, args, env.data());
            posix_spawn_file_actions_destroy(&actions);
            if (error) { child_ = -1; throw std::runtime_error("Cannot spawn wave collector: " + std::to_string(error)); }
            std::cerr << "GPU metric: wave requested, mode=" << mode << ", sample=" << sample_ << '\n';
        } catch (const std::exception &error) {
            sample_.clear();
            std::cerr << "GPU metric: sysfs (" << error.what() << ")\n";
        }
    }
    const std::string &sample_path() const { return sample_; }
    void poll() {
        if (child_ <= 0) return;
        int status{};
        if (waitpid(child_, &status, WNOHANG) == child_) {
            child_ = -1;
            std::cerr << "GPU wave collector exited (status=" << status
                      << "); using sysfs when no fresh shared sample exists\n";
        }
    }
    ~WaveCollector() {
        if (child_ <= 0) return;
        // Reap first so a completed PID cannot be reused before signaling.
        if (waitpid(child_, nullptr, WNOHANG) != 0) return;
        kill(child_, SIGTERM);
        for (unsigned i = 0; i < 100; ++i) {
            if (waitpid(child_, nullptr, WNOHANG) != 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(child_, SIGKILL);
        while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    WaveCollector(const WaveCollector &) = delete;
    WaveCollector &operator=(const WaveCollector &) = delete;
};
} // namespace amdva::dashboard
