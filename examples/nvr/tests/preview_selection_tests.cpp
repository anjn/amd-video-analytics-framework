#include "backend/preview_selection.hpp"
#include <iostream>
#include <stdexcept>
void check(bool c,const char*m){if(!c)throw std::runtime_error(m);}
int main(){try{
 amdva::nvr::detail::PreviewSelection selection;
 check(selection.contains("camera-old"),"Before the first layout, live sources are eligible");
 check(!selection.contains(""),"An empty slot cannot be visible");
 selection.set({"camera-new","resource-monitor"});
 check(!selection.contains("camera-old"),"Old source is no longer requested");
 check(selection.contains("camera-new"),"A future slot binding inherits the requested ID");
 selection.set({});
 check(!selection.contains("camera-new"),"An explicit empty layout hides all sources");
 selection.set({"camera-old"});
 check(selection.contains("camera-old")&&!selection.contains("camera-new"),"Restored assignment uses current source IDs");
 std::cout<<"Preview source intent survives asynchronous slot assignment\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
