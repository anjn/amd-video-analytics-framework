#pragma once
namespace amdva::nvr::detail {
// A swap is submission, not proof that a picture reached the screen.
// Nonblocking swaps remove the extra refresh wait; advance still requires ACK.
inline bool may_advance_preview(bool confirmed,bool retired) {
    return confirmed || retired;
}
inline bool may_reuse_preview(bool gpu_complete,bool confirmed,bool retired) {
    return gpu_complete && (confirmed || retired);
}
}
