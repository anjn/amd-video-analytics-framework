#include "presentation_feedback.hpp"
#include "amdva/diagnostics.hpp"
#include <QGuiApplication>
#include <QTimer>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include "presentation-time-client.h"
#include <algorithm>
#include <ctime>
#include <limits>
#include <map>
#include <stdexcept>

namespace amdva::nvr {
struct PresentationFeedback::Impl {
    QWindow &window;
    Callback callback;
    std::function<void(std::string)> failure;
    wl_display *display{};
    wl_event_queue *queue{};
    wl_registry *registry{};
    wp_presentation *presentation{};
    struct Request { uint64_t id;std::vector<FeedbackFrame> frames; };
    std::map<struct wp_presentation_feedback*,Request> pending;
    QTimer *timer{};
    int64_t refresh{16666667};
    clockid_t clock{-1};
    uint64_t serial{};
    std::string error;
    bool reported{};
    static void global(void *data,wl_registry *registry,uint32_t id,const char *name,uint32_t) {
        auto &self=*static_cast<Impl*>(data);
        if(std::string(name)!="wp_presentation")return;
        self.presentation=static_cast<wp_presentation*>(wl_registry_bind(
            registry,id,&wp_presentation_interface,1));
        static const wp_presentation_listener listener{
            [](void *d,wp_presentation*,uint32_t id) {
                static_cast<Impl*>(d)->clock=static_cast<clockid_t>(id);
            }};
        wp_presentation_add_listener(self.presentation,&listener,&self);
    }
    static void diagnostic(const FeedbackFrame &frame,const char *event,int64_t ns,
                           int64_t value=0,int64_t value2=0) {
        if(frame.timing.telemetry)
            frame.timing.telemetry->diagnostic(frame.timing.frame,event,value,value2,-1,ns);
    }
    int64_t converted(uint32_t hi,uint32_t lo,uint32_t nsec) {
        const uint64_t seconds=(uint64_t(hi)<<32)|lo;
        if(nsec>=1000000000 || seconds>uint64_t((std::numeric_limits<int64_t>::max()-nsec)/1000000000LL))
            throw std::runtime_error("Invalid presentation timestamp");
        const auto stamp=int64_t(seconds)*1000000000LL+nsec;
        timespec now{};
        if(clock_gettime(clock,&now)!=0)throw std::runtime_error("Presentation clock unavailable");
        const int64_t source_now=int64_t(now.tv_sec)*1000000000LL+now.tv_nsec;
        // Preserve the actual presentation clock. Only translate its epoch to
        // telemetry's monotonic domain; callback receipt is a distinct event.
        return Telemetry::monotonic_ns()+(stamp-source_now);
    }
    void finish(struct wp_presentation_feedback *feedback,bool shown,uint32_t hi=0,
                uint32_t lo=0,uint32_t ns=0,uint32_t rate=0,uint32_t flags=0) noexcept {
        try {
            auto it=pending.find(feedback);
            if(it==pending.end())throw std::runtime_error("Unknown presentation feedback identity");
            auto request=std::move(it->second);pending.erase(it);
            wp_presentation_feedback_destroy(feedback);
            const auto received=Telemetry::monotonic_ns();
            const auto at=shown?converted(hi,lo,ns):received;
            if(shown && rate>0 && rate<=1000000000)refresh=rate;
            for(const auto &frame:request.frames) {
                diagnostic(frame,shown?"presentation_commit_presented":"presentation_commit_discarded",
                           at,request.id,flags);
                diagnostic(frame,"presentation_feedback_received",received,request.id,at);
            }
            callback(request.frames,at,flags,shown);
        } catch(const std::exception &e){error=e.what();}
        catch(...){error="Unknown presentation feedback failure";}
    }
    Impl(QWindow &window,Callback callback,std::function<void(std::string)> failure)
        :window(window),callback(std::move(callback)),failure(std::move(failure)) {
        try {
        if(!QGuiApplication::platformName().startsWith("wayland"))
            throw std::runtime_error("Lossless live presentation requires a Wayland presentation-time backend");
        auto *native=QGuiApplication::platformNativeInterface();
        display=native?static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display")):nullptr;
        if(!display)throw std::runtime_error("Qt Wayland native display unavailable");
        queue=wl_display_create_queue(display);
        if(!queue)throw std::runtime_error("Cannot create presentation event queue");
        auto *wrapper=static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper),queue);
        registry=wl_display_get_registry(wrapper);wl_proxy_wrapper_destroy(wrapper);
        static const wl_registry_listener listener{global,[](void*,wl_registry*,uint32_t){}};
        wl_registry_add_listener(registry,&listener,this);
        // Startup protocol handshakes only, never per-frame GPU synchronization.
        if(wl_display_roundtrip_queue(display,queue)<0 || !presentation ||
           wl_display_roundtrip_queue(display,queue)<0 || clock<0)
            throw std::runtime_error("Wayland presentation-time feedback unavailable");
        diagnostic_event("presentation",0,0,"presentation_mode",Telemetry::monotonic_ns(),clock,1);
        timer=new QTimer(&window);
        QObject::connect(timer,&QTimer::timeout,&window,[this]{poll();});
        timer->start(5);
        } catch(...) { cleanup();throw; }
    }
    void poll() {
        if(error.empty() && wl_display_dispatch_queue_pending(display,queue)<0)
            error="Wayland presentation event dispatch failed";
        if(!error.empty() && !reported){reported=true;failure(error);}
    }
    void submit(std::vector<FeedbackFrame> frames) {
        poll();
        if(frames.empty() || !error.empty())return;
        if(pending.size()>=64){error="Presentation feedback stalled beyond bounded capacity";poll();return;}
        auto *surface=static_cast<wl_surface*>(QGuiApplication::platformNativeInterface()
            ->nativeResourceForWindow("surface",&window));
        if(!surface){error="Qt Wayland surface unavailable";poll();return;}
        auto *feedback=wp_presentation_feedback(presentation,surface);
        static const wp_presentation_feedback_listener listener{
            [](void*,struct wp_presentation_feedback*,wl_output*){},
            [](void*d,struct wp_presentation_feedback*f,uint32_t hi,uint32_t lo,uint32_t ns,
               uint32_t rate,uint32_t,uint32_t,uint32_t flags){
                static_cast<Impl*>(d)->finish(f,true,hi,lo,ns,rate,flags);
            },
            [](void*d,struct wp_presentation_feedback*f){static_cast<Impl*>(d)->finish(f,false);}
        };
        const auto id=++serial;
        for(const auto &frame:frames)
            diagnostic(frame,"presentation_commit_submit",Telemetry::monotonic_ns(),id);
        pending.emplace(feedback,Request{id,std::move(frames)});
        wp_presentation_feedback_add_listener(feedback,&listener,this);
    }
    void cleanup() {
        if(timer){timer->stop();delete timer;}
        for(auto &[feedback,request]:pending) {
            for(const auto &frame:request.frames)
                diagnostic(frame,"presentation_feedback_retired",Telemetry::monotonic_ns(),request.id);
            wp_presentation_feedback_destroy(feedback);
        }
        if(presentation)wp_presentation_destroy(presentation);
        if(registry)wl_registry_destroy(registry);
        if(queue)wl_event_queue_destroy(queue);
        timer=nullptr;presentation=nullptr;registry=nullptr;queue=nullptr;pending.clear();
    }
    ~Impl(){cleanup();}
};
PresentationFeedback::PresentationFeedback(QWindow &window,Callback callback,
    std::function<void(std::string)> failure)
    :impl_(std::make_unique<Impl>(window,std::move(callback),std::move(failure))) {}
PresentationFeedback::~PresentationFeedback()=default;
void PresentationFeedback::submit(std::vector<FeedbackFrame> frames){impl_->submit(std::move(frames));}
void PresentationFeedback::poll(){impl_->poll();}
int64_t PresentationFeedback::refresh_ns() const{return impl_->refresh;}
bool PresentationFeedback::pending(const FeedbackFrame &frame) const {
    for(const auto &[feedback,request]:impl_->pending)
        for(const auto &other:request.frames)
            if(other.slot==frame.slot && other.generation==frame.generation &&
               other.sequence==frame.sequence)return true;
    return false;
}
}
