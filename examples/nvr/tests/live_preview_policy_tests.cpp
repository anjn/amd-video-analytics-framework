#include "app/live_preview_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace amdva::nvr::detail;
void check(bool c,const char*m){if(!c)throw std::runtime_error(m);}
int main(){try{
 check(!may_advance_preview(false,false),"Swapping cannot prove physical presentation");
 check(may_advance_preview(true,false),"Actual presentation permits next picture");
 check(may_advance_preview(false,true),"Explicit lifecycle retirement permits next picture");
 check(!may_reuse_preview(true,false,false),"GPU fence cannot prove presentation");
 check(!may_reuse_preview(false,true,false),"Presentation cannot prove GPU completion");
 check(may_reuse_preview(true,true,false),"Both proofs permit reuse");
 check(may_reuse_preview(true,false,true),"Explicit retirement and fence permit reuse");
 check(may_retire_discarded(false,false,true,false),"Unshown live frame permits progress");
 check(!may_retire_discarded(false,false,true,true),"Another pending commit prevents retirement");
 check(!may_retire_discarded(true,false,true,false),"Already shown frame is not a drop");
 check(!may_reuse_preview(false,false,true),"A live drop still requires GPU fence completion");
 std::cout<<"Live presentation admission and surface reuse contracts passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
