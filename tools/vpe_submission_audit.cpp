#define AMDVA_VPE_AUDIT_IMPLEMENTATION
#include "amdva/vpe_audit.hpp"
#include <amdgpu_drm.h>
#include <drm.h>
#include <va/va.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/ioctl.h>
#include <utility>
#include <vector>
namespace {
using VaKey=std::pair<VADisplay,unsigned>;
using FenceKey=std::pair<int,unsigned>;
uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
template<class F> F original(const char *name){return reinterpret_cast<F>(dlsym(RTLD_NEXT,name));}
void maximum(std::atomic<uint64_t>& value,uint64_t next){auto old=value.load();while(old<next&&!value.compare_exchange_weak(old,next)) {}}
struct Context;
struct Scope {
    std::shared_ptr<Context> context;
    uint64_t token{},started{},accepted{};
    unsigned target{};uint32_t flags{};
    bool own_begin{},own_completion{},synchronizing{};
};
struct Context {VADisplay display{};unsigned id{};bool alive{true},audited{};std::shared_ptr<Scope> active;};
struct Fence {
    uint64_t created{},sequence{},claimed{};
    bool pending{true},vpe{},accepted{};
    std::condition_variable returned;
};
struct State {
    std::mutex registry;
    std::set<VaKey> configs;
    std::map<VaKey,std::shared_ptr<Context>> contexts;
    // Bounded per-live syncobj metadata; never images or command payloads.
    std::map<FenceKey,std::shared_ptr<Fence>> fences;
    std::atomic<bool> registry_failed{false};
    std::atomic<uint64_t> serial{},active{},foreign{},scopes{},completed{},failed{},aborted{};
    std::atomic<uint64_t> submissions{},rejected{},wait_ns{},max_wait{},scope_ns{},max_scope{};
};
State& state(){static auto *s=new State;return *s;}
thread_local std::shared_ptr<Scope> local_scope;
void poison(State& s){++s.foreign;}
}
extern "C" unsigned amdva_vpe_audit_version(){return 1;}
extern "C" uint64_t amdva_vpe_scope_begin(void *display,uint32_t context,uint32_t target,uint32_t *failure){
    auto &s=state();++s.scopes;uint32_t flags=0;
    std::lock_guard<std::mutex> lock(s.registry);
    auto found=s.contexts.find({display,context});
    if(found==s.contexts.end()||s.registry_failed)flags|=AMDVA_VPE_CONTEXT_UNKNOWN;
    else if(found->second->active)flags|=AMDVA_VPE_CONTEXT_BUSY;
    // Concurrent writes to one output would make its completion ambiguous.
    for(const auto &entry:s.contexts)
        if(entry.second->active && entry.first.first==display && entry.second->active->target==target)
            flags|=AMDVA_VPE_CONTEXT_BUSY;
    if(local_scope)flags|=AMDVA_VPE_SCOPE_MISMATCH;
    if(s.foreign)flags|=AMDVA_VPE_FOREIGN_OPERATION;
    if(flags){if(failure)*failure=flags;++s.failed;return 0;}
    try {
        auto scope=std::make_shared<Scope>();scope->context=found->second;
        scope->target=target;scope->started=now();scope->token=++s.serial;
        scope->context->audited=true;scope->context->active=scope;local_scope=scope;++s.active;
        if(failure)*failure=0;return scope->token;
    }catch(...){++s.failed;s.registry_failed=true;if(failure)*failure=AMDVA_VPE_METADATA_INVALID;return 0;}
}
extern "C" uint32_t amdva_vpe_scope_end(uint64_t token,int completed){
    auto scope=local_scope;
    if(!scope||!token||scope->token!=token)return AMDVA_VPE_SCOPE_MISMATCH;
    auto &s=state();uint32_t flags=scope->flags;
    if(!scope->own_begin)flags|=AMDVA_VPE_BEGIN_MISSING;
    if(!scope->accepted)flags|=AMDVA_VPE_SUBMISSION_MISSING;
    if(!scope->own_completion)flags|=AMDVA_VPE_COMPLETION_MISSING;
    if(!completed)flags|=AMDVA_VPE_ABORTED;
    if(s.foreign)flags|=AMDVA_VPE_FOREIGN_OPERATION;
    if(s.registry_failed)flags|=AMDVA_VPE_METADATA_INVALID;
    {std::lock_guard<std::mutex> lock(s.registry);scope->context->active.reset();}
    const auto duration=now()-scope->started;s.scope_ns+=duration;maximum(s.max_scope,duration);
    if(flags)++s.failed;else ++s.completed;if(!completed)++s.aborted;
    --s.active;local_scope.reset();return flags;
}
extern "C" int amdva_vpe_audit_snapshot(AmdvaVpeAuditStats *out,unsigned size){
    if(!out||size!=sizeof(*out))return 0;auto &s=state();
    *out={s.scopes.load(),s.completed.load(),s.failed.load(),s.aborted.load(),s.submissions.load(),s.rejected.load(),s.foreign.load(),s.wait_ns.load(),s.max_wait.load(),s.scope_ns.load(),s.max_scope.load(),s.active.load()};return 1;
}
extern "C" VAStatus vaCreateConfig(VADisplay d,VAProfile p,VAEntrypoint entry,VAConfigAttrib *a,int n,VAConfigID *id){
    static auto call=original<decltype(&vaCreateConfig)>("vaCreateConfig");if(!call)return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto result=call(d,p,entry,a,n,id);
    if(result==VA_STATUS_SUCCESS&&entry==VAEntrypointVideoProc){auto &s=state();try{std::lock_guard<std::mutex> lock(s.registry);s.configs.insert({d,*id});}catch(...){s.registry_failed=true;}}
    return result;
}
extern "C" VAStatus vaDestroyConfig(VADisplay d,VAConfigID id){
    static auto call=original<decltype(&vaDestroyConfig)>("vaDestroyConfig");if(!call)return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto result=call(d,id);if(result==VA_STATUS_SUCCESS){auto &s=state();std::lock_guard<std::mutex> lock(s.registry);s.configs.erase({d,id});}return result;
}
extern "C" VAStatus vaCreateContext(VADisplay d,VAConfigID config,int w,int h,int flags,VASurfaceID *targets,int n,VAContextID *id){
    static auto call=original<decltype(&vaCreateContext)>("vaCreateContext");if(!call)return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto result=call(d,config,w,h,flags,targets,n,id);
    if(result==VA_STATUS_SUCCESS){auto &s=state();try{std::lock_guard<std::mutex> lock(s.registry);if(s.configs.count({d,config})){auto context=std::make_shared<Context>();context->display=d;context->id=*id;s.contexts[{d,*id}]=std::move(context);}}catch(...){s.registry_failed=true;}}
    return result;
}
extern "C" VAStatus vaDestroyContext(VADisplay d,VAContextID id){
    auto &s=state();{
        std::lock_guard<std::mutex> lock(s.registry);auto found=s.contexts.find({d,id});
        if(found!=s.contexts.end()){
            if(found->second->active){poison(s);return VA_STATUS_ERROR_OPERATION_FAILED;}
            found->second->alive=false;s.contexts.erase(found);
        }
    }
    static auto call=original<decltype(&vaDestroyContext)>("vaDestroyContext");return call?call(d,id):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaBeginPicture(VADisplay d,VAContextID context,VASurfaceID target){
    auto &s=state();auto scope=local_scope;
    const bool own=scope && scope->context->display==d && scope->context->id==context && scope->target==target;
    {
        std::lock_guard<std::mutex> lock(s.registry);
        auto found=s.contexts.find({d,context});
        if(found!=s.contexts.end()) {
            if(own)scope->own_begin=true;
            else {
                // Other VA users (e.g. encoder CSC and the metrics renderer)
                // are outside this scoped VPE contract. They may proceed only
                // if they cannot write to its context or an in-flight target.
                bool conflict=found->second->audited || bool(found->second->active);
                for(const auto &entry:s.contexts)
                    if(entry.first.first==d && entry.second->active && entry.second->active->target==target)
                        conflict=true;
                if(conflict){poison(s);return VA_STATUS_ERROR_OPERATION_FAILED;}
            }
        }
    }
    static auto call=original<decltype(&vaBeginPicture)>("vaBeginPicture");return call?call(d,context,target):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaSyncSurface(VADisplay d,VASurfaceID target){
    static auto call=original<decltype(&vaSyncSurface)>("vaSyncSurface");if(!call)return VA_STATUS_ERROR_OPERATION_FAILED;
    auto scope=local_scope;
    const bool own=scope&&scope->context->display==d&&scope->target==target;
    if(own)scope->synchronizing=true;
    const auto result=call(d,target);
    if(own){scope->synchronizing=false;if(result==VA_STATUS_SUCCESS)scope->own_completion=true;}
    return result;
}
// Mesa submits asynchronously and can share a kernel context between VA
// contexts. The output binary syncobj, not that context or CPU thread, links
// accepted VPE CS to the caller's EXISTING target-surface completion wait.
extern "C" int ioctl(int fd,unsigned long request,...) noexcept {
    void *argument=nullptr;
    if(request!=FIOCLEX&&request!=FIONCLEX&&request!=TIOCNOTTY&&request!=TIOCSBRK&&request!=TIOCCBRK){va_list args;va_start(args,request);argument=va_arg(args,void*);va_end(args);}
    static auto call=original<decltype(&ioctl)>("ioctl");if(!call){errno=ENOSYS;return -1;}
    const int incoming_errno=errno;auto &s=state();
    std::shared_ptr<Fence> submission,waiting;
    FenceKey waited_key{};bool vpe=false;
    auto scope=local_scope;
    try {
        if(s.active&&request==DRM_IOCTL_AMDGPU_CS&&argument){
            const auto *cs=static_cast<const drm_amdgpu_cs*>(argument);
            bool other=false;std::vector<unsigned> signals;
            if(cs->in.num_chunks>128||(!cs->in.chunks&&cs->in.num_chunks))s.registry_failed=true;
            else {
                auto *pointers=reinterpret_cast<const uint64_t*>(uintptr_t(cs->in.chunks));
                for(unsigned i=0;i<cs->in.num_chunks;++i){
                    if(!pointers[i]){s.registry_failed=true;break;}
                    auto *chunk=reinterpret_cast<const drm_amdgpu_cs_chunk*>(uintptr_t(pointers[i]));
                    if(!chunk->chunk_data||chunk->length_dw>16384){s.registry_failed=true;break;}
                    if(chunk->chunk_id==AMDGPU_CHUNK_ID_IB){
                        if(chunk->length_dw<sizeof(drm_amdgpu_cs_chunk_ib)/4){s.registry_failed=true;break;}
                        auto *ib=reinterpret_cast<const drm_amdgpu_cs_chunk_ib*>(uintptr_t(chunk->chunk_data));
                        vpe|=ib->ip_type==AMDGPU_HW_IP_VPE;other|=ib->ip_type!=AMDGPU_HW_IP_VPE;
                    }else if(chunk->chunk_id==AMDGPU_CHUNK_ID_SYNCOBJ_OUT){
                        if(chunk->length_dw*4%sizeof(drm_amdgpu_cs_chunk_sem)){s.registry_failed=true;break;}
                        auto *out=reinterpret_cast<const drm_amdgpu_cs_chunk_sem*>(uintptr_t(chunk->chunk_data));
                        for(unsigned j=0;j<chunk->length_dw*4/sizeof(*out);++j)signals.push_back(out[j].handle);
                    }
                }
                submission=std::make_shared<Fence>();submission->created=now();submission->vpe=vpe&&!other;
                std::lock_guard<std::mutex> lock(s.registry);
                for(auto handle:signals){
                    if(s.fences.size()>=16384&&!s.fences.count({fd,handle})){s.registry_failed=true;break;}
                    s.fences[{fd,handle}]=submission;
                }
            }
        }
        if(request==DRM_IOCTL_SYNCOBJ_WAIT&&argument&&scope&&scope->synchronizing){
            const auto *wait=static_cast<const drm_syncobj_wait*>(argument);
            // One exact binary fence. Unsupported wait layouts never prove VPE.
            if(wait->count_handles==1&&wait->handles){
                waited_key={fd,*reinterpret_cast<const uint32_t*>(uintptr_t(wait->handles))};
                std::lock_guard<std::mutex> lock(s.registry);auto found=s.fences.find(waited_key);
                if(found!=s.fences.end())waiting=found->second;
            }
        }
        if(argument&&(request==DRM_IOCTL_SYNCOBJ_DESTROY||request==DRM_IOCTL_SYNCOBJ_RESET)){
            std::lock_guard<std::mutex> lock(s.registry);
            if(request==DRM_IOCTL_SYNCOBJ_DESTROY)s.fences.erase({fd,static_cast<const drm_syncobj_destroy*>(argument)->handle});
            else {
                const auto *array=static_cast<const drm_syncobj_array*>(argument);
                if(array->count_handles<=16384&&array->handles){auto *handles=reinterpret_cast<const uint32_t*>(uintptr_t(array->handles));for(unsigned i=0;i<array->count_handles;++i)s.fences.erase({fd,handles[i]});}
                else if(s.active)s.registry_failed=true;
            }
        }
    }catch(...){s.registry_failed=true;}
    errno=incoming_errno;
    const auto result=call(fd,request,argument);const int saved_errno=errno;
    if(submission){
        std::lock_guard<std::mutex> lock(s.registry);
        submission->accepted=result==0;submission->pending=false;
        if(result==0)submission->sequence=static_cast<const drm_amdgpu_cs*>(argument)->out.handle;
        if(vpe){if(result==0)++s.submissions;else ++s.rejected;}
        submission->returned.notify_all();
    }
    if(waiting&&result==0){
        const auto before=now();std::unique_lock<std::mutex> lock(s.registry);
        // The GPU wait has ALREADY succeeded. Only finish CPU observer metadata.
        waiting->returned.wait(lock,[&]{return !waiting->pending;});
        const auto duration=now()-before;s.wait_ns+=duration;maximum(s.max_wait,duration);
        auto current=s.fences.find(waited_key);
        if(current!=s.fences.end()&&current->second==waiting&&waiting->created>=scope->started&&waiting->vpe&&!waiting->claimed){
            if(waiting->accepted&&waiting->sequence){waiting->claimed=scope->token;++scope->accepted;}
            else scope->flags|=AMDVA_VPE_SUBMISSION_REJECTED;
        }
    }
    errno=saved_errno;return result;
}
