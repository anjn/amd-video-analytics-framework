// Fake driver for the audit contract test only. Never installed or used by NVR.
#include <amdgpu_drm.h>
#include <va/va.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <sys/ioctl.h>
static std::atomic<unsigned> next_id{1};
extern "C" VAStatus vaCreateConfig(VADisplay,VAProfile,VAEntrypoint,VAConfigAttrib*,int,VAConfigID *out) {*out=next_id++;return VA_STATUS_SUCCESS;}
extern "C" VAStatus vaDestroyConfig(VADisplay,VAConfigID) {return VA_STATUS_SUCCESS;}
extern "C" VAStatus vaCreateContext(VADisplay,VAConfigID,int,int,int,VASurfaceID*,int,VAContextID *out) {*out=next_id++;return VA_STATUS_SUCCESS;}
extern "C" VAStatus vaDestroyContext(VADisplay,VAContextID) {return VA_STATUS_SUCCESS;}
extern "C" VAStatus vaBeginPicture(VADisplay,VAContextID,VASurfaceID) {return VA_STATUS_SUCCESS;}
extern "C" VAStatus vaSyncSurface(VADisplay,VASurfaceID target) {
    if(target==999)return VA_STATUS_ERROR_OPERATION_FAILED;
    uint32_t handle=100+target;drm_syncobj_wait wait{};wait.handles=uintptr_t(&handle);wait.count_handles=1;
    return ioctl(4242,DRM_IOCTL_SYNCOBJ_WAIT,&wait)==0?VA_STATUS_SUCCESS:VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" int ioctl(int fd,unsigned long request,...) noexcept {
    va_list args;va_start(args,request);void *argument=va_arg(args,void*);va_end(args);
    if(request==DRM_IOCTL_AMDGPU_CS) {
        auto *cs=static_cast<drm_amdgpu_cs *>(argument);
        if(fd==4243||cs->in.flags==0x31415926){errno=EIO;return -1;}
        cs->out.handle=123;return 0;
    }
    if(request==DRM_IOCTL_SYNCOBJ_WAIT)return 0;
    if(request==DRM_IOCTL_SYNCOBJ_RESET||request==DRM_IOCTL_SYNCOBJ_DESTROY)return 0;
    if(request==FIOCLEX)return argument==nullptr?0:-1;
    if(request==TCXONC)return uintptr_t(argument)==42?0:-1;
    if(request==_IOWR('X',1,uint64_t)) {*static_cast<uint64_t *>(argument)^=0x12345678;return 0;}
    errno=ENOTTY;return -1;
}
