#pragma once
#include "amdva/vpe_audit.hpp"
#include <stdexcept>
#include <string>
namespace amdva::detail {
// One existing VA VideoProc operation. The auditor matches accepted VPE
// submissions to the existing target fence; models remain concurrent.
class VpeAuditScope {
    uint64_t token_{};
    uint32_t context_,target_;
  public:
    VpeAuditScope(void *display,uint32_t context,uint32_t target):context_(context),target_(target){
        if(!amdva_vpe_audit_version||amdva_vpe_audit_version()!=1||!amdva_vpe_scope_begin||!amdva_vpe_scope_end)
            throw std::runtime_error("VPE submission auditor ABI1 is required; build the framework and source scripts/env.sh before launch");
        uint32_t failure=0;token_=amdva_vpe_scope_begin(display,context,target,&failure);
        if(!token_)throw std::runtime_error("Cannot begin VPE execution audit: flags="+std::to_string(failure)+" context="+std::to_string(context));
    }
    VpeAuditScope(const VpeAuditScope&)=delete;
    VpeAuditScope& operator=(const VpeAuditScope&)=delete;
    ~VpeAuditScope(){if(token_)amdva_vpe_scope_end(token_,0);}
    void complete(){
        const auto token=token_;const auto failure=amdva_vpe_scope_end(token,1);token_=0;
        if(failure)throw std::runtime_error("VPE submission/completion audit failed: flags="+std::to_string(failure)+" context="+std::to_string(context_)+" target="+std::to_string(target_)+" operation="+std::to_string(token));
    }
};
}
