#pragma once
#include "amdva/diagnostics.hpp"
#include "amdva/copy_audit.hpp"
#include "amdva/vpe_audit.hpp"
#include "backend/controller.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <thread>
namespace amdva::nvr {
class DiagnosticsSession {
    std::atomic<bool> running_{true};
    std::thread thread_;
  public:
    explicit DiagnosticsSession(Controller &controller) {
        const char *directory=std::getenv("AMDVA_DIAGNOSTICS_DIR");
        if(!directory || !*directory) return;
        std::filesystem::create_directories(directory);
        const std::string path = std::string(directory) + "/status.jsonl";
        if(std::filesystem::exists(path)) throw std::runtime_error("Diagnostic status already exists");
        thread_ = std::thread([this, &controller, path] {
            try {
            std::ofstream output(path);
            while (running_) {
                const auto snapshot_start=Telemetry::monotonic_ns();
                QJsonArray channels, timing, ai;
                for (const auto &s : controller.status())
                    channels.append(QJsonObject{{"id",QString::fromStdString(s.source.id)},
                        {"slot",s.slot},{"live",s.connection=="live"},{"recording",QString::fromStdString(s.recording)},
                        {"decoded",double(s.decoded)},{"ai",double(s.ai)},
                        {"display_fps",s.display_fps},{"ai_fps",s.ai_fps},
                        {"gaps",double(s.gaps)},{"generation",double(s.generation)},
                        {"error",QString::fromStdString(s.error)}});
                for (const auto &s : controller.telemetry())
                    timing.append(QJsonDocument::fromJson(QByteArray::fromStdString(s.json())).object());
                for(const auto &m:controller.ai_runtime()->snapshot())
                    ai.append(QJsonObject{{"stage",QString::fromStdString(m.stage)},
                      {"camera",QString::fromStdString(m.camera)},{"completed",double(m.completed)},
                      {"pending",int(m.pending)},{"dropped",double(m.dropped)},
                      {"p95_ms",m.p95_ms},{"wait_ms",m.wait_ms},{"mean_batch",m.mean_batch}});
                QJsonObject search;
                if(auto *service=controller.search_service()) {
                    const auto s=service->status();
                    search=QJsonObject{{"state",QString::fromStdString(s.state)},
                        {"error",QString::fromStdString(s.error)},
                        {"tracking_state",QString::fromStdString(s.tracking_state)},
                        {"tracking_error",QString::fromStdString(s.tracking_error)},
                        {"stored",double(s.stored)},{"pending",double(s.pending)},
                        {"dropped",double(s.dropped)}};
                }
                QJsonObject copy_audit{{"active",controller.copy_audit_started_ns()>0},
                    {"start_ns",double(controller.copy_audit_started_ns())},
                    {"violations",double(amdva_audit_errors?amdva_audit_errors():0)},
                    {"copies",double(amdva_audit_copies?amdva_audit_copies():0)}};
                AmdvaVpeAuditStats v{};
                const bool available=amdva_vpe_audit_snapshot && amdva_vpe_audit_snapshot(&v,sizeof(v));
                QJsonObject vpe_audit{{"available",available},{"scopes",double(v.scopes)},
                    {"completed",double(v.completed)},{"failed",double(v.failed)},{"aborted",double(v.aborted)},
                    {"submissions",double(v.submissions)},{"rejected",double(v.rejected_submissions)},
                    {"foreign",double(v.foreign_operations)},{"active",double(v.active_scope)}};
                QJsonObject row{{"vpe_audit",vpe_audit},{"copy_audit",copy_audit},{"search",search},{"snapshot_duration_ms",double(Telemetry::monotonic_ns()-snapshot_start)/1e6},{"ai",ai},{"ns",double(Telemetry::monotonic_ns())},{"channels",channels},
                                {"telemetry",timing},{"fatal",QString::fromStdString(controller.fatal())}};
                output << QJsonDocument(row).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
                if (!output) { std::cerr << "Diagnostic status write failed\n"; return; }
                for (unsigned i=0; i<50 && running_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            } catch(const std::exception &e) { std::cerr << "Diagnostic status failed: " << e.what() << "\n"; }
        });
    }
    ~DiagnosticsSession() { running_=false; if(thread_.joinable()) thread_.join(); }
};
} // namespace amdva::nvr
