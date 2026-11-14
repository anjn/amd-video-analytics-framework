#include "window.hpp"
#include "diagnostics_session.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTimer>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace amdva::nvr;
// Explicit hardware test; never registered in hardware-free CTest runs.
int main(int argc,char **argv) {
    if(argc!=3){std::cerr<<"Usage: amdva-presentation-lifecycle CONFIG.json RESULT.json\n";return 2;}
    if(setenv("AMDVA_COPY_AUDIT","1",1)!=0)return 2;
    initialize_runtime();
    QSurfaceFormat format;format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3,0);format.setSwapInterval(1);QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);install_shutdown_signals(app);
    QJsonArray phases;std::string error;bool completed=false,copy_was_active=false;
    try {
        amdva::diagnostics_enabled();auto options=load_config(argv[1]);
        std::array<std::string,16> cells{};unsigned count=0;std::string extra;
        for(const auto &source:options.sources) {
            if(source.selected && count<16)cells[count++]=source.id;
            else if(!source.selected && extra.empty())extra=source.id;
        }
        if(count!=16 || extra.empty())throw std::runtime_error("Requires 16 selected and one extra source");
        Controller controller(options);DiagnosticsSession diagnostics(controller);
        Tiles view(controller);view.resize(1600,900);view.show();
        QObject::connect(&app,&QCoreApplication::aboutToQuit,[&]{controller.request_shutdown();});
        auto *tiles=&view;tiles->layout(cells,16);
        using Stamp=std::pair<uint64_t,uint64_t>;
        std::map<std::string,Stamp> before;
        std::array<uint64_t,16> before_presented{};
        auto snapshot=[&](const char *phase,bool require_progress) {
            QJsonArray slot_states;std::map<std::string,Stamp> next;std::set<std::string> observed;
            unsigned slot_index=0;
            for(auto &slot:controller.gpu_slots()) {
                const auto index=slot_index++;
                std::lock_guard<std::mutex> lock(slot.mutex);
                if(slot.channel.empty())continue;
                if(slot.preview_visible.load())observed.insert(slot.channel);
                if(require_progress && slot.preview_visible.load()) {
                    auto old=before.find(slot.channel);
                    const bool same=old!=before.end() && old->second.first==slot.generation;
                    if(same && slot.sequence<old->second.second)
                        throw std::runtime_error("Preview sequence reversed: "+slot.channel);
                    const auto delta=tiles->presented_by_slot(index)-before_presented[index];
                    const uint64_t minimum=slot.channel=="resource-monitor"?90:270;
                    if(delta<minimum)throw std::runtime_error("Preview did not recover: "+slot.channel);
                }
                next[slot.channel]={slot.generation,slot.sequence};
                before_presented[index]=tiles->presented_by_slot(index);
                QJsonArray states;for(const auto &p:slot.preview)states.append(p.state);
                slot_states.append(QJsonObject{{"source",QString::fromStdString(slot.channel)},
                    {"presented",double(tiles->presented_by_slot(index))},{"generation",double(slot.generation)},{"sequence",double(slot.sequence)},
                    {"visible",slot.preview_visible.load()},{"planes",states}});
            }
            if(require_progress && observed!=std::set<std::string>(cells.begin(),cells.end()))
                throw std::runtime_error("Visible source membership did not recover");
            phases.append(QJsonObject{{"phase",phase},{"ns",double(amdva::Telemetry::monotonic_ns())},
                {"presented",double(tiles->presented())},{"slots",slot_states}});
            amdva::diagnostic_event("presentation-test",0,0,"presentation_test_phase",amdva::Telemetry::monotonic_ns(),phases.size());
            before=std::move(next);
        };
        unsigned stage=0;auto at=std::chrono::steady_clock::now();QTimer timer;
        const std::string original=cells[0];
        QObject::connect(&timer,&QTimer::timeout,[&] {
            try {
                if(!controller.fatal().empty())throw std::runtime_error(controller.fatal());
                if(!tiles->error().empty())throw std::runtime_error(tiles->error());
                const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-at).count();
                const double wait=(stage==0?20:(stage==1||stage==3||stage==5?3:15));
                if(elapsed<wait)return;
                switch(stage) {
                case 0:snapshot("warmup",true);tiles->hold_live(true);break;
                case 1:snapshot("held",false);tiles->hold_live(false);break;
                case 2:snapshot("hold-resumed",true);view.hide();break;
                case 3:snapshot("hidden",false);view.show();break;
                case 4:snapshot("hide-resumed",true);tiles->layout(cells,16,0);break;
                case 5:snapshot("focused",false);tiles->layout(cells,16);break;
                case 6:snapshot("grid-resumed",true);controller.select(original,false);
                    controller.select(extra,true);cells[0]=extra;tiles->layout(cells,16);break;
                case 7:snapshot("camera-switched",true);controller.select(extra,false);
                    controller.select(original,true);cells[0]=original;tiles->layout(cells,16);break;
                case 8:snapshot("camera-restored",true);completed=true;controller.request_shutdown();app.quit();return;
                }
                ++stage;at=std::chrono::steady_clock::now();
            }catch(const std::exception &e){error=e.what();app.quit();}
        });timer.start(100);app.exec();
        copy_was_active=controller.copy_audit_started_ns()>0;
    }catch(const std::exception &e){error=e.what();}
    AmdvaVpeAuditStats v{};
    const bool vpe_available=amdva_vpe_audit_snapshot && amdva_vpe_audit_snapshot(&v,sizeof(v));
    const bool vpe_complete=vpe_available && v.scopes && v.scopes==v.completed &&
        !v.failed && !v.aborted && !v.rejected_submissions && !v.foreign_operations && !v.active_scope;
    const bool copy_proved=copy_was_active && amdva_audit_errors && amdva_audit_errors()==0;
    if(error.empty() && (!copy_proved || !vpe_complete))error="Missing or incomplete copy/VPE audit";
    const QJsonObject result{{"passed",completed&&error.empty()},
        {"copy_audit_active",copy_was_active},{"copy_audit_passed",copy_proved},
        {"vpe_audit_complete",vpe_complete},{"vpe_scopes",double(v.scopes)},
        {"error",QString::fromStdString(error)},{"phases",phases},
        {"scope","Declared interactive lifecycle trial; not a fixed-layout physical-frame acceptance run"}};
    std::ofstream out(argv[2]);out<<QJsonDocument(result).toJson().constData();
    if(!out){std::cerr<<"Cannot save lifecycle evidence\n";return 2;}
    std::cout<<(completed&&error.empty()?"Presentation lifecycle passed\n":"Presentation lifecycle failed\n");
    return completed&&error.empty()?0:1;
}
