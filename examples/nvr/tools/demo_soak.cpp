#include "window.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTimer>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <dlfcn.h>
#include <iterator>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 4) { std::cerr << "Usage: amdva-demo-soak CONFIG SECONDS OUTPUT.jsonl\n"; return 2; }
    initialize_runtime();
    QSurfaceFormat format; format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    install_shutdown_signals(app);
    int result = 1;
    try {
        const int seconds = std::stoi(argv[2]);
        if (seconds < 30) throw std::runtime_error("Duration must be >=30 seconds");
        auto audit_start=reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_start"));
        auto audit_stop=reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_stop"));
        auto audit_errors=reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_errors"));
        if(!audit_start || !audit_stop || !audit_errors)
            throw std::runtime_error("Soak requires the framework copy auditor");
        bool auditing=false, search_verified=false;
        uint64_t search_request=0;
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]); window.showFullScreen();
        std::ofstream log(argv[3]);
        if (!log) throw std::runtime_error("Cannot open soak output");
        auto start = Clock::now();
        std::vector<double> rss_samples; std::vector<size_t> fd_samples;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            try {
                double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
                if (!controller.fatal().empty()) throw std::runtime_error(controller.fatal());
                unsigned long pages=0, resident=0;
                std::ifstream("/proc/self/statm") >> pages >> resident;
                double rss=resident * double(sysconf(_SC_PAGESIZE));
                size_t fds=std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator());
                rss_samples.push_back(rss); fd_samples.push_back(fds);
                if(!auditing && elapsed > std::min(60., seconds/3.) && controller.inference_ready()) {
                    audit_start(); auditing=true;
                }
                if(audit_errors()) throw std::runtime_error("Copy auditor reported prohibited transfers");
                QJsonArray channels;
                for (const auto &s : controller.status()) {
                    channels.push_back(QJsonObject{{"id",QString::fromStdString(s.source.id)},
                        {"connection",QString::fromStdString(s.connection)},
                        {"error",QString::fromStdString(s.error)}, {"received",double(s.received)},
                        {"decoded",double(s.decoded)}, {"ai",double(s.ai)},
                        {"display_fps",s.display_fps}, {"recording",QString::fromStdString(s.recording)}});
                    if (elapsed > 120 && (s.connection != "live" || !s.error.empty() || s.received==0))
                        throw std::runtime_error("Camera failure: " + s.source.id + ": " + s.error);
                }
                QJsonObject row{{"seconds",elapsed},{"rss_bytes",rss},{"fds",double(fds)},
                    {"channels",channels},{"inference_ready",controller.inference_ready()},
                    {"presented",double(window.live_view()->presented())},
                    {"recordings",double(controller.recordings().size())},
                    {"audit_active",auditing}, {"audit_errors",double(audit_errors())}};
                if (auto *search=controller.search_service()) {
                    auto s=search->status();
                    row.insert("search",QJsonObject{{"state",QString::fromStdString(s.state)},
                        {"error",QString::fromStdString(s.error)},
                        {"tracking_error",QString::fromStdString(s.tracking_error)}, {"stored",double(s.stored)}});
                    if(!search_request && s.stored>0) search_request=search->text("a person");
                    if(search_request) {
                        auto r=search->result();
                        if(r.request==search_request) {
                            if(!r.error.empty()) throw std::runtime_error("Text search: " + r.error);
                            search_verified=true;
                        }
                    }
                    row.insert("search_query_completed",search_verified);
                    if (elapsed>120 && (!s.error.empty() || !s.tracking_error.empty()))
                        throw std::runtime_error("Search/tracking failure: " + s.error + s.tracking_error);
                }
                QJsonArray ai;
                for (const auto &m : controller.ai_runtime()->snapshot())
                    if(m.camera.empty()) ai.push_back(QJsonObject{{"stage",QString::fromStdString(m.stage)},
                        {"completed",double(m.completed)}, {"mean_ms",m.mean_ms}, {"p95_ms",m.p95_ms}});
                row.insert("ai",ai);
                log << QJsonDocument(row).toJson(QJsonDocument::Compact).toStdString() << std::endl;
                if (elapsed>=seconds) {
                    if (!controller.inference_ready() || !auditing || !window.live_view()->presented() || controller.recordings().empty())
                        throw std::runtime_error("Inference/rendering/recording/audit acceptance incomplete");
                    if(seconds>=1200 && !search_verified)
                        throw std::runtime_error("Search was not exercised; check models and video content");
                    audit_stop(); auditing=false;
                    if (seconds>=1200) {
                        // Compare steady-state windows after warm-up, not startup allocations.
                        size_t n=rss_samples.size(), begin=std::min(size_t(60),n/3), width=std::min(size_t(60),n/4);
                        auto median=[](std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];};
                        double base=median({rss_samples.begin()+begin,rss_samples.begin()+begin+width});
                        double tail=median({rss_samples.end()-width,rss_samples.end()});
                        if(tail-base>std::max(512.0*1024*1024,base*.25))
                            throw std::runtime_error("Steady-state RSS grew beyond 512 MiB / 25% allowance");
                        if(fd_samples.back()>fd_samples[begin]+64)
                            throw std::runtime_error("Steady-state FD growth exceeds 64");
                    }
                    log << "{\"passed\":true,\"duration_seconds\":" << elapsed << "}" << std::endl;
                    result=0; app.quit();
                }
            } catch(const std::exception &e) {
                log << QJsonDocument(QJsonObject{{"passed",false},{"error",QString::fromUtf8(e.what())}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
                std::cerr << e.what() << std::endl; result=1; app.quit();
            }
        });
        timer.start(5000); app.exec(); audit_stop(); controller.shutdown();
    } catch (const std::exception &e) {std::cerr << e.what() << std::endl;return 1;}
    return result;
}
