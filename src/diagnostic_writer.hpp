#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace amdva::detail {
struct DiagnosticRecord {
    std::array<char, 64> source{};
    std::array<char, 40> event{};
    uint64_t generation{}, sequence{};
    int64_t ns{}, value{}, value2{}, pts{-1};
};
class DiagnosticWriter {
    std::filesystem::path directory_;
    std::vector<DiagnosticRecord> ring_;
    size_t head_{}, queued_{};
    uint64_t accepted_{}, lost_{}, written_{}, limit_lost_{}, bytes_{};
    uint64_t byte_limit_;
    bool stopping_{}, closed_{}, failed_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::ofstream stream_;
    void run() {
        try {
            std::vector<DiagnosticRecord> batch;
            batch.reserve(1024);
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping_ || queued_ >= 1024; });
                    if (!queued_ && stopping_) break;
                    batch.clear();
                    while (queued_ && batch.size() < 1024) {
                        batch.push_back(ring_[head_]);
                        head_ = (head_ + 1) % ring_.size();
                        --queued_;
                    }
                }
                for (const auto &r : batch) {
                    std::ostringstream line;
                    line << r.ns << ',' << std::quoted(r.source.data()) << ',' << r.generation << ','
                         << r.sequence << ',' << std::quoted(r.event.data()) << ',' << r.value << ','
                         << r.value2 << ',' << r.pts << '\n';
                    const auto text = line.str();
                    if (bytes_ + text.size() > byte_limit_) { ++limit_lost_; continue; }
                    stream_ << text;
                    if (!stream_) throw std::runtime_error("Diagnostic write failed");
                    bytes_ += text.size();
                    ++written_;
                }
                stream_.flush();
                if (!stream_) throw std::runtime_error("Diagnostic flush failed");
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_ = true;
        }
    }
  public:
    DiagnosticWriter(const std::filesystem::path &path, size_t capacity = 65536,
                     uint64_t byte_limit = 4294967296ULL)
        : directory_(path), ring_(capacity), byte_limit_(byte_limit) {
        if (!capacity) throw std::invalid_argument("Diagnostic queue capacity is zero");
        std::filesystem::create_directories(directory_);
        if (std::filesystem::exists(directory_ / "events.csv"))
            throw std::runtime_error("Diagnostic run directory already used");
        stream_.open(directory_ / "events.csv");
        if (!stream_) throw std::runtime_error("Cannot open diagnostic output");
        stream_ << "ns,source,generation,sequence,event,value,value2,pts\n";
        thread_ = std::thread([this] { run(); });
    }
    void record_loss() { std::lock_guard<std::mutex> lock(mutex_); ++lost_; }
    bool push(const DiagnosticRecord &r) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || failed_ || queued_ == ring_.size()) { ++lost_; return false; }
        ring_[(head_ + queued_) % ring_.size()] = r;
        ++queued_;
        ++accepted_;
        if (queued_ >= 1024) cv_.notify_one();
        return true;
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
        stream_.close();
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream summary(directory_ / "diagnostics-summary.json");
        summary << "{\"accepted\":" << accepted_ << ",\"written\":" << written_
                << ",\"queue_lost\":" << lost_ << ",\"limit_lost\":" << limit_lost_
                << ",\"bytes\":" << bytes_ << ",\"write_failed\":" << (failed_ ? "true" : "false")
                << ",\"pending\":" << queued_ << "}\n";
        closed_ = true;
    }
    ~DiagnosticWriter() { close(); }
};
} // namespace amdva::detail
