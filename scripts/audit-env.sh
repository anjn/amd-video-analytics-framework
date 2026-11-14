#!/usr/bin/env bash
# Source after env.sh, or again with --copy immediately before an audited launch.
# Ignore inherited preload paths; select only this checkout's built modules.
unset LD_PRELOAD AMDVA_VPE_AUDIT_LIBRARY AMDVA_COPY_AUDIT_LIBRARY
mapfile -t _amdva_auditors < <(/usr/bin/python3 -I "$AMDVA_ROOT/scripts/audit_env.py")
if [[ ${#_amdva_auditors[@]} != 2 ]]; then
    echo 'Cannot resolve the framework audit libraries.' >&2
    unset _amdva_auditors
    return 1
fi
export AMDVA_VPE_AUDIT_LIBRARY="${_amdva_auditors[0]}"
export AMDVA_COPY_AUDIT_LIBRARY="${_amdva_auditors[1]}"
if [[ -n "$AMDVA_VPE_AUDIT_LIBRARY" ]]; then export LD_PRELOAD="$AMDVA_VPE_AUDIT_LIBRARY"; fi
if [[ ${1:-} == --copy && ${AMDVA_COPY_AUDIT:-0} == 1 ]]; then
    if [[ -z "$AMDVA_COPY_AUDIT_LIBRARY" ]]; then
        echo 'Build libamdva_copy_audit before requesting copy auditing.' >&2
        return 1
    fi
    export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$AMDVA_COPY_AUDIT_LIBRARY"
fi
unset _amdva_auditors
