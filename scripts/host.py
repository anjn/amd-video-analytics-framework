#!/usr/bin/env python3
"""Validate the host GPU SDK; never install or select a different SDK silently."""
import argparse
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "dependencies.lock.json").read_text())["host"]


def clean_env():
    env = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONPATH", "PYTHONHOME",
                "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "CMAKE_PREFIX_PATH", "GST_PLUGIN_PATH",
                "GST_PLUGIN_SYSTEM_PATH", "HSA_OVERRIDE_GFX_VERSION"):
        env.pop(key, None)
    return env


def choose(explicit, candidates, marker):
    if explicit:
        root = Path(explicit).resolve()
        if not (root / marker).is_file():
            raise ValueError(f"Invalid SDK root: {root}; missing {marker}")
        return root
    matches = list(dict.fromkeys(p.resolve() for p in map(Path, candidates) if (p / marker).is_file()))
    if len(matches) != 1:
        raise ValueError(f"Expected one SDK installation, found {matches}. Set AMDVA_ROCM_ROOT / AMDVA_MIGRAPHX_ROOT explicitly")
    return matches[0]


def inspect(devices=True, build=True):
    release = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines() if "=" in line)
    release = {k: v.strip('"') for k, v in release.items()}
    if release.get("ID") != "ubuntu" or release.get("VERSION_ID") not in LOCK["ubuntu"]:
        raise ValueError("Supported OS: Ubuntu 24.04 or 26.04")
    rocm = choose(os.environ.get("AMDVA_ROCM_ROOT"), ["/opt/rocm/core-10.1", "/opt/rocm-10.1.0", "/opt/rocm"], ".info/version")
    version = (rocm / ".info/version").read_text().strip().split("-")[0]
    if version != LOCK["rocm"]:
        raise ValueError(f"ROCm {version}: required {LOCK['rocm']}")
    mx = choose(os.environ.get("AMDVA_MIGRAPHX_ROOT"), ["/opt/rocm/extras-10", str(rocm)], "lib/cmake/migraphx/migraphx-config-version.cmake")
    text = (mx / "lib/cmake/migraphx/migraphx-config-version.cmake").read_text()
    found = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', text)
    if not found or found[1] != LOCK["migraphx"]:
        raise ValueError(f"MIGraphX must be {LOCK['migraphx']}")
    for base, files in [(rocm, ["include/hip/hip_runtime.h", "lib/libamdhip64.so", "lib/libhiprtc.so", "lib/libroctx64.so.4"]),
                        (mx, ["include/migraphx/migraphx.hpp", "lib/libmigraphx_c.so", "lib/migraphx/lib/libmigraphx_gpu.so"])]:
        for name in files:
            if not (base / name).is_file():
                raise ValueError(f"Missing SDK file: {base / name}")
    env = clean_env()
    env["LD_LIBRARY_PATH"] = f"{rocm}/lib:{mx}/lib:{mx}/lib/migraphx/lib"
    for lib in [rocm / "lib/libamdhip64.so", mx / "lib/migraphx/lib/libmigraphx_gpu.so"]:
        output = subprocess.check_output(["ldd", str(lib)], env=env, text=True)
        if "not found" in output:
            raise ValueError(f"Unresolved dependencies for {lib}:\n{output}")
    if devices:
        for path in ["/dev/kfd", "/dev/dri/renderD128"]:
            if not os.access(path, os.R_OK | os.W_OK):
                raise ValueError(f"No read/write access: {path}; check render/video groups")
    packages = {}
    for package in (LOCK["packages"] if build else []):
        query = subprocess.run(["dpkg-query", "-W", "-f=${db:Status-Status} ${Version}", package], capture_output=True, text=True)
        if query.returncode or not query.stdout.startswith("installed "):
            raise ValueError(f"Missing host prerequisite {package}; see scripts/host.py --packages")
        packages[package] = query.stdout.split(" ", 1)[1]
    tools = {}
    if build:
        for tool in ("cargo", "rustc"):
            if not shutil.which(tool):
                raise ValueError(f"Missing {tool}: install Rust/Cargo >=1.85 via native packages or rustup")
            tool_version = subprocess.check_output([tool, "--version"], text=True).strip()
            match = re.search(r"(\d+)\.(\d+)\.(\d+)", tool_version)
            if not match or tuple(map(int, match.groups())) < (1, 85, 0):
                raise ValueError(f"{tool} >=1.85 is required; use a suitable distro package or rustup toolchain")
            tools[tool] = tool_version
    sdk_packages = {}
    installed = subprocess.run(["dpkg-query", "-W", "-f=${binary:Package}\t${db:Status-Status}\t${Version}\n", "amdrocm*"], capture_output=True, text=True)
    for line in installed.stdout.splitlines():
        fields=line.split("\t")
        if len(fields)==3 and fields[1]=="installed":
            sdk_packages[fields[0]]=fields[2]
    return {"sdk_packages": sdk_packages, "tools": tools, "packages": packages, "os": release["VERSION_ID"], "rocm": version, "migraphx": found[1],
            "rocm_root": str(rocm), "migraphx_root": str(mx)}


def inspect_ort(venv, os_version, npu=True):
    venv = venv.resolve()
    if not (venv / "onnxruntime/include/onnxruntime/onnxruntime_cxx_api.h").is_file():
        raise ValueError("Ryzen AI 1.9 C++ SDK missing in selected environment")
    env = clean_env()
    if os_version == "26.04":
        env["GLIBC_TUNABLES"] = "glibc.rtld.execstack=2"
    info = json.loads(subprocess.check_output([str(venv / "bin/python"), "-c",
        "import json,onnxruntime as o; print(json.dumps({'version':o.__version__,'providers':o.get_available_providers()}))"], env=env, text=True))
    if info["version"] != "1.30.0" or "CPUExecutionProvider" not in info["providers"]:
        raise ValueError("Ryzen AI 1.9 ORT 1.30.0 with CPU text execution is required")
    result = {"ryzen_ai_venv": str(venv), "onnxruntime": info["version"]}
    if npu:
        if "VitisAIExecutionProvider" not in info["providers"]:
            raise ValueError("NPU mode requires VitisAIExecutionProvider")
        memlock = resource.getrlimit(resource.RLIMIT_MEMLOCK)[0]
        if memlock != resource.RLIM_INFINITY and memlock < 64 * 1024 * 1024:
            raise ValueError("NPU needs memlock >=64 MiB; configure PAM limits and log in again")
        xrt = subprocess.check_output(["/opt/xilinx/xrt/bin/xrt-smi", "examine"], text=True, stderr=subprocess.STDOUT)
        if not re.search(r"Version\s*:\s*2\.26\.89\b", xrt):
            raise ValueError("XRT 2.26.89 with a recognized NPU is required")
        result["xrt"] = "2.26.89"
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--runtime", action="store_true", help="Check runtime SDKs without requiring build packages")
    p.add_argument("--packages", action="store_true", help="Print host apt prerequisite packages")
    p.add_argument("--no-devices", action="store_true", help="Only for build-only CI hosts")
    p.add_argument("--ryzen-ai-venv", type=Path)
    p.add_argument("--ort-only", action="store_true", help="Check CPU text/GPU MobileCLIP SDK without requiring XRT or NPU access")
    p.add_argument("--write", action="store_true", help="Record selected SDK roots for env.sh")
    args = p.parse_args()
    if args.packages:
        print(" ".join(LOCK["packages"]))
        return 0
    try:
        result = inspect(not args.no_devices, not args.runtime)
        if args.ort_only and not args.ryzen_ai_venv:
            raise ValueError("--ort-only requires --ryzen-ai-venv")
        if args.ryzen_ai_venv:
            result["ort" if args.ort_only else "npu"] = inspect_ort(args.ryzen_ai_venv, result["os"], not args.ort_only)
        if args.write:
            target = ROOT / ".deps/host.json"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Host prerequisite error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
