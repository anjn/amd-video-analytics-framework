// Show the actual camera wall and require live ReID even when embedding is unavailable.
#include "window.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <algorithm>
#include <fstream>
#include <iostream>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: amdva-reid-demo-validate CONFIG SECONDS OUTPUT.jsonl\n";
        return 2;
    }
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int result = 0;
    try {
        auto options = load_config(argv[1]);
        const unsigned seconds = std::stoul(argv[2]);
        if (seconds < 10 || options.search.reid_model.empty())
            throw std::runtime_error("ReID model and at least 10 seconds required");
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.resize(1280, 900);
        window.show();
        auto *search = controller.search_service();
        if (!search)
            throw std::runtime_error("Search/Tracking required");
        std::ofstream log(argv[3]);
        if (!log)
            throw std::runtime_error("Cannot open log");
        const auto started = Clock::now();
        auto last_progress = started;
        uint64_t previous_calls{};
        unsigned fresh_people_peak{};
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            try {
                if (!controller.fatal().empty())
                    throw std::runtime_error(controller.fatal());
                auto state = search->status();
                if (state.tracking_state != "ready" || !state.tracking_error.empty())
                    throw std::runtime_error("Tracking: " + state.tracking_error);
                uint64_t calls{};
                for (auto &m : controller.ai_runtime()->snapshot())
                    if (m.camera.empty() && m.stage == "ReID inference + transfer")
                        calls = m.completed;
                const auto now = Clock::now();
                if (calls > previous_calls)
                    last_progress = now;
                previous_calls = calls;
                QJsonArray cameras;
                unsigned fresh_people{};
                for (const auto &source : controller.status()) {
                    const auto tracks = search->tracks(source.source.id);
                    const double age =
                        tracks.generation
                            ? std::chrono::duration<double, std::milli>(now - tracks.captured)
                                  .count()
                            : -1;
                    const bool fresh = tracks.generation && age <= 1000;
                    if (fresh)
                        fresh_people += tracks.objects.size();
                    cameras.append(QJsonObject{{"id", QString::fromStdString(source.source.id)},
                                               {"fresh", fresh},
                                               {"people", int(tracks.objects.size())},
                                               {"age_ms", age}});
                }
                fresh_people_peak = std::max(fresh_people_peak, fresh_people);
                const double elapsed = std::chrono::duration<double>(now - started).count();
                log << QJsonDocument(
                           QJsonObject{
                               {"elapsed", elapsed},
                               {"reid_calls", double(calls)},
                               {"tracking_state", QString::fromStdString(state.tracking_state)},
                               {"tracking_error", QString::fromStdString(state.tracking_error)},
                               {"embedding_state", QString::fromStdString(state.state)},
                               {"embedding_error", QString::fromStdString(state.error)},
                               {"fresh_people", int(fresh_people)},
                               {"cameras", cameras}})
                           .toJson(QJsonDocument::Compact)
                           .constData()
                    << '\n';
                log.flush();
                if (now - last_progress > std::chrono::seconds(10))
                    throw std::runtime_error("ReID inference stopped for 10 seconds");
                if (elapsed >= seconds) {
                    if (!calls || !fresh_people_peak || !fresh_people)
                        throw std::runtime_error(
                            "No fresh confirmed person IDs on the camera wall");
                    if (!window.live_view()->presented())
                        throw std::runtime_error("Camera wall did not present frames");
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[3]) + ".png"));
                    std::cout << "PASS: ReID calls=" << calls << " fresh people=" << fresh_people
                              << " embedding=" << state.state << '\n';
                    timer.stop();
                    // Also inspect the actual statistics widget, not just backend counters.
                    auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
                    tabs->setCurrentIndex(
                        tabs->indexOf(window.findChild<QWidget *>("ai-statistics")));
                    window.findChild<QPushButton *>("ai-view-detailed")->click();
                    QTimer::singleShot(1500, &app, [&, calls] {
                        auto *table = window.findChild<QTableWidget *>("ai-metrics");
                        uint64_t shown{};
                        if (table && table->columnCount() == 13)
                            for (int row = 0; row < table->rowCount(); ++row)
                                if (table->item(row, 0) && table->item(row, 1) &&
                                    table->item(row, 0)->text() == "ReID inference + transfer" &&
                                    table->item(row, 1)->text() == "All" && table->item(row, 12))
                                    shown = table->item(row, 12)->text().toULongLong();
                        if (shown < calls) {
                            std::cerr << "ReID cumulative count missing from statistics widget\n";
                            result = 1;
                        } else {
                            std::cout << "PASS: statistics widget Completed (total)=" << shown
                                      << '\n';
                        }
                        window.grab().save(
                            QString::fromStdString(std::string(argv[3]) + ".statistics.png"));
                        app.quit();
                    });
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                result = 1;
                timer.stop();
                app.quit();
            }
        });
        timer.start(500);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return result;
}
