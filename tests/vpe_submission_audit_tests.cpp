#include "amdva/vpe_audit.hpp"
#include <amdgpu_drm.h>
#include <va/va.h>
#include <cerrno>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <thread>
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
static void submit(unsigned ip,unsigned kernel_context,unsigned handle,bool reject=false){
    drm_amdgpu_cs_chunk_ib ib{};ib.ip_type=ip;
    drm_amdgpu_cs_chunk_sem signal{};signal.handle=handle;
    drm_amdgpu_cs_chunk chunks[2]{};
    chunks[0].chunk_id=AMDGPU_CHUNK_ID_IB;chunks[0].length_dw=sizeof(ib)/4;chunks[0].chunk_data=uintptr_t(&ib);
    chunks[1].chunk_id=AMDGPU_CHUNK_ID_SYNCOBJ_OUT;chunks[1].length_dw=sizeof(signal)/4;chunks[1].chunk_data=uintptr_t(&signal);
    uint64_t pointers[]={uintptr_t(&chunks[0]),uintptr_t(&chunks[1])};
    drm_amdgpu_cs cs{};cs.in.ctx_id=kernel_context;cs.in.num_chunks=2;cs.in.chunks=uintptr_t(pointers);cs.in.flags=reject?0x31415926:0;
    errno=EDOM;const auto result=ioctl(4242,DRM_IOCTL_AMDGPU_CS,&cs);
    require(reject?(result==-1&&errno==EIO):(result==0&&cs.out.handle==123&&errno==EDOM),"Submission arguments/return/errno changed");
}

int main(int argc,char **argv){
    try{
        require(amdva_vpe_audit_version&&amdva_vpe_audit_version()==1,"Preload auditor ABI1");
        const std::string mode=argc>1?argv[1]:"general";
        auto display=reinterpret_cast<VADisplay>(uintptr_t(0x1000));VAConfigID config{};VAContextID a{},b{};
        vaCreateConfig(display,VAProfileNone,VAEntrypointVideoProc,nullptr,0,&config);
        vaCreateContext(display,config,16,16,0,nullptr,0,&a);vaCreateContext(display,config,16,16,0,nullptr,0,&b);
        auto target=[&](unsigned owner){return owner==a?7u:8u;};
        auto begin=[&](unsigned owner){uint32_t flags=0;auto token=amdva_vpe_scope_begin(display,owner,target(owner),&flags);require(token&&flags==0,"Registered scope must start");return token;};
        auto complete=[&](uint64_t token,unsigned owner,unsigned ip=AMDGPU_HW_IP_VPE,bool sync=true,bool reject=false){
            require(vaBeginPicture(display,owner,target(owner))==VA_STATUS_SUCCESS,"Own VA operation begin");
            submit(ip,42,100+target(owner),reject);if(sync)vaSyncSurface(display,target(owner));return amdva_vpe_scope_end(token,1);
        };
        if(mode=="first-compute"){
            require(complete(begin(a),a,AMDGPU_HW_IP_COMPUTE)&AMDVA_VPE_SUBMISSION_MISSING,"First-use compute cannot register VPE");
            require(complete(begin(a),a)==0,"Failed first registration releases admission");
        }else{
            require(complete(begin(a),a)==0,"Initial VPE registration and completion");
            if(mode=="auxiliary-va"){
                require(vaBeginPicture(display,b,target(b))==VA_STATUS_SUCCESS,"Independent auxiliary VPP must stay usable");
                const auto token=begin(a);vaBeginPicture(display,a,target(a));
                require(vaBeginPicture(display,b,target(b))==VA_STATUS_SUCCESS,"Distinct auxiliary target may run concurrently");
                submit(AMDGPU_HW_IP_COMPUTE,42,100+target(a));submit(AMDGPU_HW_IP_VPE,42,100+target(b));
                vaSyncSurface(display,target(a));
                require(amdva_vpe_scope_end(token,1)&AMDVA_VPE_SUBMISSION_MISSING,"Auxiliary VPE cannot prove audited compute operation");
                require(complete(begin(a),a)==0,"Audited VPE still works after independent auxiliary VPP");
                require(vaBeginPicture(display,a,target(a))==VA_STATUS_ERROR_OPERATION_FAILED,"Audited context cannot bypass its scope outside active work");
            }else if(mode=="foreign-va"){
                const auto token=begin(a);
                require(vaBeginPicture(display,b,target(a))==VA_STATUS_ERROR_OPERATION_FAILED,"Unscoped write to audited target must be rejected");
                require(complete(token,a)&AMDVA_VPE_FOREIGN_OPERATION,"Another VA operation cannot prove own work");
            }else if(mode=="alias-context"){
                const auto token=begin(b);vaBeginPicture(display,b,target(b));submit(AMDGPU_HW_IP_VPE,42,100+target(b));vaSyncSurface(display,target(b));
                require(amdva_vpe_scope_end(token,1)==0,"Shared kernel contexts use distinct exact completion fences");
            }else{
                require(complete(begin(b),b)==0,"Second independent VPE context registration");
                if(mode=="foreign-cs"){
                    const auto token=begin(a);vaBeginPicture(display,a,target(a));std::thread worker([&]{submit(AMDGPU_HW_IP_VPE,42,100+target(b));});worker.join();vaSyncSurface(display,target(a));
                    require(amdva_vpe_scope_end(token,1)&AMDVA_VPE_SUBMISSION_MISSING,"Other target fence cannot prove own work, even with shared kernel context");
                }else{
                    require(complete(begin(a),a,AMDGPU_HW_IP_COMPUTE)&AMDVA_VPE_SUBMISSION_MISSING,"Compute is not VPE");
                    require(complete(begin(a),a,AMDGPU_HW_IP_VPE,false)&AMDVA_VPE_COMPLETION_MISSING,"Observe own completion");
                    require(complete(begin(a),a,AMDGPU_HW_IP_VPE,true,true)&AMDVA_VPE_SUBMISSION_REJECTED,"Kernel rejection fails");
                    // A prior VPE fence must not prove a new operation or a reset handle.
                    auto token=begin(a);vaBeginPicture(display,a,target(a));vaSyncSurface(display,target(a));
                    require(amdva_vpe_scope_end(token,1)&AMDVA_VPE_SUBMISSION_MISSING,"Stale reused fence proved new scope");
                    token=begin(a);vaBeginPicture(display,a,target(a));require(amdva_vpe_scope_end(token,0)&AMDVA_VPE_ABORTED,"Abort releases context");
                    require(complete(begin(a),a)==0,"No deadlock after abort");
                    // Real Mesa submits on a worker; exact fence correspondence must work across threads.
                    token=begin(a);vaBeginPicture(display,a,target(a));
                    std::thread submitter([&]{submit(AMDGPU_HW_IP_VPE,42,100+target(a));});submitter.join();
                    vaSyncSurface(display,target(a));require(amdva_vpe_scope_end(token,1)==0,"Asynchronous worker submission provenance");
                    token=begin(a);require(amdva_vpe_scope_end(token+1,0)&AMDVA_VPE_SCOPE_MISMATCH,"Wrong token cannot end an operation");
                    std::promise<void> entered;auto ready=entered.get_future();std::exception_ptr error;
                    std::thread second([&]{try{auto t=begin(b);entered.set_value();require(complete(t,b)==0,"Concurrent independent proof");}catch(...){error=std::current_exception();try{entered.set_value();}catch(...){}}});
                    uint32_t overlap=0;
                    require(!amdva_vpe_scope_begin(display,b,target(a),&overlap) &&
                            (overlap&AMDVA_VPE_CONTEXT_BUSY),"Shared output surface must be rejected");
                    const bool concurrent=ready.wait_for(std::chrono::milliseconds(200))==std::future_status::ready;
                    const auto first=complete(token,a);second.join();if(error)std::rethrow_exception(error);
                    require(concurrent&&first==0,"Registered models stay concurrent without shared GPU lease");
                    uint64_t value=0xabcdef;require(ioctl(42,_IOWR('X',1,uint64_t),&value)==0&&value==(0xabcdef^0x12345678),"Pointer ioctl forwarding");
                    require(ioctl(42,TCXONC,42)==0&&ioctl(42,FIOCLEX)==0,"Scalar/argument-free ioctl forwarding");
                }
            }
        }
        AmdvaVpeAuditStats stats{};require(amdva_vpe_audit_snapshot(&stats,sizeof(stats))&&!stats.active_scope,"All scopes drain");
        vaDestroyContext(display,b);vaDestroyContext(display,a);vaDestroyConfig(display,config);
        std::cout<<mode<<": VPE provenance/completion and lifecycle passed\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
