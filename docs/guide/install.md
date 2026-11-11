<!-- source: docs/guide/install.ja.md ec3463c226ce7adae9bfdb5de08620d7d29d692e -->
# Installation

Build dependencies and prepare models from a Git checkout. No `.deps`, binaries, virtual environments or compiled model caches from another PC are required. First-time setup needs network access; users supply the videos.

## Host prerequisites

| Item | Requirement |
|---|---|
| OS | Ubuntu 24.04 LTS or 26.04 LTS, x86_64 |
| GPU | Strix Point gfx1150 or Strix Halo gfx1151 |
| GPU SDK | ROCm **10.1.0**, MIGraphX **2.18.0**; no automatic alternate-version fallback |
| Graphics | Host kernel, Mesa, libva and libdrm; verify hardware with the probe |
| Access | Read/write permission on `/dev/kfd` and `/dev/dri/renderD128`; configure render/video groups and log in again if needed |
| Tools | Python 3.12+ and native development packages; uv resolves Python 3.12 for CPU model export |

Administrators install the ROCm 10.1.0 runtime, headers and compiler plus MIGraphX 2.18.0 using the [AMD instructions](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/install/rocm.html) for the OS/GPU. Bootstrap never installs or upgrades the host SDK.

Observed packages: `amdrocm-core10.1-gfx1151=10.1.0-3`, `amdrocm10-migraphx=2.18.0-1`; select the matching architecture package for gfx1150. The observed repositories are `https://stable.repo.amd.com/rocm/core/packages/ubuntu2604/` and `https://stable.repo.amd.com/rocm/migraphx/packages/ubuntu2604/`; use the OS-specific `ubuntu2404` repository on 24.04. Administrators must verify and retain the required versions even when stable repository defaults change.

Default roots are `/opt/rocm/core-10.1` and `/opt/rocm/extras-10`. For a custom installation, set `AMDVA_ROCM_ROOT` and `AMDVA_MIGRAPHX_ROOT` before `host.py --write`. Ambiguous installations, mismatched versions, missing headers or unresolved dynamic dependencies fail explicitly.

## Build and check

```bash
# Run after the administrator has installed ROCm 10.1.0 and MIGraphX 2.18.0.
sudo apt-get update
sudo apt-get install $(python3 scripts/host.py --packages)
python3 scripts/host.py --write
python3 scripts/bootstrap.py --jobs 8
source scripts/env.sh
build/amdva-probe
```

`host.py` checks the OS, SDK versions, development packages, dynamic dependencies and GPU permissions. `.deps/host.json` records selected roots and host package versions. Confirm `gate_passed:true` from `amdva-probe`; this prerequisite alone does not prove whole-pipeline zero-copy.

Bootstrap verifies pinned GStreamer sources and patches and builds into `.deps/prefix`. It does not extract another Ubuntu version's ROCm, Qt or libva packages. It is rerunnable; `--check` verifies cached source downloads. Source `scripts/env.sh` in each shell. Foreign Torch, alternate ROCm and legacy sysroot search paths are not inherited.

## Optional NPU

Administrators prepare Ryzen AI SW **1.9**, XRT **2.26.89** and the distributed NPU driver using the [AMD instructions](https://ryzenai.docs.amd.com/en/latest/linux.html). Keep credentials and licenses outside Git. If Secure Boot rejects the driver, the administrator decides enrollment or disabling; no automatic standard-driver substitution is performed.

**AMD officially targets Ubuntu 24.04; framework testing on 26.04 does not extend vendor support.** On 26.04 the explicit NPU activation shell applies the glibc compatibility setting needed by the ORT executable-stack requirement. Resolve inadequate memlock through PAM limits and a fresh login.

```bash
python3 scripts/host.py --write --ryzen-ai-venv /path/to/rai19-venv
python3 scripts/bootstrap.py --jobs 8 --ryzen-ai-venv /path/to/rai19-venv
source scripts/env.sh
```

## Models and videos

Follow the fresh preparation workflow in the [1440p demo](../../examples/nvr/demo/1440p/README.md). Models are generated from locked public checkpoints. Old GPU/SDK artifacts are rejected; NPU caches are generated at their final location.

## Validation status

The target matrix is both OS versions × both GPUs. Consult the [validation matrix](../dev/support-matrix.md) to distinguish targets from tested configurations. No OS replacement is performed automatically.

Rust / Cargo >=1.85 is required for the locked tokenizer dependencies. A working rustup toolchain is accepted instead of the distribution cargo package; host.py records the actual tool versions.

## Administrator apt setup example

For a new host, select the correct GPU below. This example uses the package versions observed during validation and registers the OS-specific signed repositories. It is a manual administrator action, not part of framework bootstrap. Review existing repository configuration before applying it; do not replace an existing SDK without approval.

```bash
set -euo pipefail
. /etc/os-release
case "$VERSION_ID" in
  24.04) distro=ubuntu2404 ;;
  26.04) distro=ubuntu2604 ;;
  *) echo "Ubuntu 24.04 or 26.04 required" >&2; exit 1 ;;
esac
gpu=gfx1151 # Strix Halo; use gfx1150 for Strix Point
case "$gpu" in gfx1150|gfx1151) ;; *) exit 1 ;; esac
key=$(mktemp)
curl -fsSL https://stable.repo.amd.com/rocm/gpg/packages.gpg -o "$key"
fingerprint=$(gpg --show-keys --with-colons "$key" | awk -F: '$1=="fpr" {print $10; exit}')
test "$fingerprint" = D0F004A0025A1145C7807FCD0701EAC4D5E02107
sudo install -d -m 0755 /etc/apt/keyrings
gpg --dearmor < "$key" | sudo tee /etc/apt/keyrings/amdrocm.gpg >/dev/null
rm "$key"
sudo tee /etc/apt/sources.list.d/amdrocm-stable.sources >/dev/null <<EOF
Types: deb
URIs: https://stable.repo.amd.com/rocm/core/packages/$distro/
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/amdrocm.gpg
EOF
sudo tee /etc/apt/sources.list.d/amdrocm-migraphx.sources >/dev/null <<EOF
Types: deb
URIs: https://stable.repo.amd.com/rocm/migraphx/packages/$distro/
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/amdrocm.gpg
EOF
sudo apt-get update
sudo apt-get install "amdrocm-core10.1-$gpu=10.1.0-3" amdrocm10-migraphx=2.18.0-1
```

The apt prerequisite list intentionally omits cargo: keep an existing working rustup installation. If Rust is absent or too old, have the administrator install Rust/Cargo >=1.85 through the OS packages or [rustup](https://rustup.rs/), then rerun host.py.

For MobileCLIP GPU image inference and CPU text inference without NPU access, use
`--ryzen-ai-venv /path/to/rai19-venv --mobileclip-backend gpu` with bootstrap.
The ORT C++ SDK and CPU provider remain required, but this mode does not require
XRT, the Vitis provider, or NPU memlock/device access. Prepare the demo with
`--backend gpu`. Omitting the SDK argument explicitly disables MobileCLIP/NPU
in the build instead of inheriting stale CMake options.
