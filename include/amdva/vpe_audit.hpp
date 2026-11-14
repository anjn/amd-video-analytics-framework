#pragma once
#include <stdint.h>
// Load libamdva_vpe_audit.so before libva/Mesa (scripts/env.sh does this).
// A scope contains one VideoProc operation and its EXISTING surface completion.
// No pixels, command payloads or GPU performance counters are read.
#ifdef AMDVA_VPE_AUDIT_IMPLEMENTATION
#define AMDVA_VPE_WEAK
#else
#define AMDVA_VPE_WEAK __attribute__((weak))
#endif
enum AmdvaVpeFailure {
    AMDVA_VPE_CONTEXT_UNKNOWN = 1u << 0,
    AMDVA_VPE_FOREIGN_OPERATION = 1u << 1,
    AMDVA_VPE_BEGIN_MISSING = 1u << 2,
    AMDVA_VPE_SUBMISSION_MISSING = 1u << 3,
    AMDVA_VPE_SUBMISSION_REJECTED = 1u << 4,
    AMDVA_VPE_COMPLETION_MISSING = 1u << 5,
    AMDVA_VPE_ABORTED = 1u << 6,
    AMDVA_VPE_METADATA_INVALID = 1u << 7,
    AMDVA_VPE_SCOPE_MISMATCH = 1u << 8,
    AMDVA_VPE_CONTEXT_ALIASED = 1u << 9,
    AMDVA_VPE_CONTEXT_BUSY = 1u << 10
};
struct AmdvaVpeAuditStats {
    uint64_t scopes, completed, failed, aborted;
    uint64_t submissions, rejected_submissions, foreign_operations;
    uint64_t wait_ns, maximum_wait_ns, scope_ns, maximum_scope_ns;
    uint64_t active_scope; // Number of operations currently in flight.
};
extern "C" {
AMDVA_VPE_WEAK unsigned amdva_vpe_audit_version();
AMDVA_VPE_WEAK uint64_t amdva_vpe_scope_begin(void *display, uint32_t context,
                                            uint32_t target, uint32_t *failure);
// completed=1 only after the caller's ordinary vaSyncSurface succeeded.
// Always ends/releases the scope, including on errors. Zero means verified.
AMDVA_VPE_WEAK uint32_t amdva_vpe_scope_end(uint64_t token, int completed);
AMDVA_VPE_WEAK int amdva_vpe_audit_snapshot(AmdvaVpeAuditStats *out, unsigned size);
}
#undef AMDVA_VPE_WEAK
