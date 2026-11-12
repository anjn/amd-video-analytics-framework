#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace amdva::detail {
// A cycle-weighted ratio over the returned hardware regions, not ALU issue time.
inline std::optional<double> wave_presence(double elapsed, const std::vector<double> &busy) {
    if (!std::isfinite(elapsed) || elapsed <= 0 || elapsed >= 4294967295. || busy.empty())
        return {};
    double sum = 0;
    for (double value : busy) {
        if (!std::isfinite(value) || value < 0 || value > elapsed)
            return {};
        sum += value / elapsed;
    }
    return 100. * sum / busy.size();
}

// Private, bounded, versioned protocol shared by the helper and ResourceMonitor.
inline std::optional<double> read_wave_sample(const std::string &text, const std::string &pci,
                                               const std::string &boot, int64_t now,
                                               std::string &status) {
    status = "invalid wave sample";
    if (text.empty()) { status = "wave sample unavailable"; return {}; }
    if (text.size() > 32768) return {};
    std::istringstream input(text);
    std::string magic, device, boot_id;
    int64_t timestamp{};
    double elapsed{};
    unsigned count{};
    if (!(input >> magic >> device >> boot_id >> timestamp >> elapsed >> count) ||
        magic != "AMDVA_WAVE_V1" || device != pci || boot.empty() || boot_id != boot ||
        count == 0 || count > 1024)
        return {};
    if (timestamp <= 0 || timestamp > now || now - timestamp > 3000000000LL) {
        status = "wave sample expired";
        return {};
    }
    std::vector<double> busy(count);
    for (auto &value : busy) if (!(input >> value)) return {};
    input >> std::ws;
    if (!input.eof()) return {};
    auto result = wave_presence(elapsed, busy);
    status = result ? "ok" : "invalid wave counters";
    return result;
}
} // namespace amdva::detail
