# Host-SDK validation matrix

Target baseline: ROCm 10.1.0 / MIGraphX 2.18.0, with the host's Mesa/libva/libdrm.
A target is not marked validated until fresh construction and hardware acceptance pass.

| OS | GPU | Fresh build/contracts | GPU probe | Long-run demo |
|---|---|---|---|---|
| Ubuntu 24.04.5 | Strix Point gfx1150 | Fresh build and 30 contracts passed, kernel 6.17.0-1033-oem | Passed: VA/VPE/HIP aliasing | Not run; short wave checks are recorded separately |
| Ubuntu 24.04 | Strix Halo gfx1151 | Pending host availability | Pending | Pending |
| Ubuntu 26.04 | Strix Point gfx1150 | Pending host availability | Pending | Pending |
| Ubuntu 26.04.1 | Strix Halo gfx1151 | Passed on x86-084-maple, kernel 7.0.0-43 | Passed: VA/VPE/HIP aliasing | Passed 20 minutes (user-approved shortened run) |

The old Ubuntu 24.04/gfx1150 results used ROCm 7.2/MIGraphX 2.15 and do not validate this baseline.
NPU requires separately installed Ryzen AI SW 1.9 / XRT 2.26.89 and vendor driver.
AMD's Ryzen AI Linux OS support remains Ubuntu 24.04; 26.04 results are framework evidence only.
No CPU fallback is accepted as hardware validation. Probe success is not full-pipeline copy-audit proof.

The user shortened acceptance to 20 minutes before a network disconnect. An external monitor evaluated 1204.9 seconds of samples and stopped the launcher: zero copy-audit errors, median RSS 1956→1976 MiB, FD227 stable, search/recording/all 17 inputs and catalog integrity passed. The VCN capacity correction is checked in a subsequent short run; loaded libraries were not replaced during the long run.


## Presentation FIFO comparison scope (2026-11-14)

The Ubuntu 24.04/gfx1150 input-to-decode investigation later passed two 61-minute
normal runs at `1c38061`. This is separate from the uncommitted presentation FIFO:
its Qt scheduling comparison passed 15 minutes, all-frame Wayland reconciliation,
copy/VPE audits, and interactive recovery checks on that reference host only.
Current 47 CPU contracts are not a new fresh-checkout validation. The new physical
feedback path is unvalidated on the other three OS/GPU combinations and X11;
previous rows must not be read as validating this new path. See the
[per-trial evidence and limits](nvr/input-decode-latency.md#experimental-decoded-presentation-fifo-2026-11-14).
