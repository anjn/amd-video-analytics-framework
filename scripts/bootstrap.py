#!/usr/bin/env python3
"""Install locked dependencies to a host-local prefix, without sudo."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".deps"


def run(*command, env=None, cwd=ROOT):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), env=env, cwd=cwd, check=True)


def verify(path, checksum):
    if not path.is_file():
        return False
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest() == checksum


def download(url, path, checksum):
    if not verify(path, checksum):
        temporary = path.with_suffix(path.suffix + ".download")
        urllib.request.urlretrieve(url, temporary)
        if not verify(temporary, checksum):
            temporary.unlink()
            raise RuntimeError(f"SHA256 mismatch: {url}")
        temporary.replace(path)


def extract_source(archive, target, checksum):
    """Publish only a complete extraction; retain interrupted source trees."""
    marker = target / ".amdva-source-sha256"
    if marker.is_file() and marker.read_text().strip() == checksum:
        return False
    with tempfile.TemporaryDirectory(prefix=".extract-", dir=target.parent) as stage:
        with tarfile.open(archive) as tar:
            tar.extractall(stage, filter="data")
        source = Path(stage) / target.name
        if not source.is_dir() or source.is_symlink():
            raise RuntimeError(f"Unexpected source archive layout: {archive}")
        (source / marker.name).write_text(checksum + "\n")
        if target.exists():
            target.rename(target.with_name(target.name + f".unverified-{time.time_ns()}"))
        source.replace(target)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Verify cached downloads without installing")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--ryzen-ai-venv", type=Path, help="Enable MobileCLIP/NPU using a host-prepared Ryzen AI 1.9 environment")
    parser.add_argument("--mobileclip-backend", choices=["gpu", "npu", "gpu+npu"], default="npu")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    if args.check:
        files = [(DEPS / "src" / Path(a["url"]).name, a["sha256"]) for a in lock["gstreamer"]]
        missing = [str(path) for path, checksum in files if not verify(path, checksum)]
        print(json.dumps({"passed": not missing, "checked": len(files), "invalid": missing}))
        return 2 if missing else 0
    host_args = ["--ryzen-ai-venv", args.ryzen_ai_venv] if args.ryzen_ai_venv else []
    if args.ryzen_ai_venv and args.mobileclip_backend == "gpu":
        host_args.append("--ort-only")
    run(sys.executable, ROOT / "scripts/host.py", "--write", *host_args)
    for tool in ("cmake", "pkg-config", "git", "m4", "cc", "c++", "ninja"):
        if not shutil.which(tool):
            raise RuntimeError(f"Required host tool not installed: {tool}")
    for directory in ("src", "prefix"):
        (DEPS / directory).mkdir(parents=True, exist_ok=True)
    # A repository-local tool environment avoids requiring an unpinned host uv.
    tools = DEPS / "tools"
    if not (tools / "bin/python").exists():
        run(sys.executable, "-m", "venv", tools)
    run(tools / "bin/python", "-m", "pip", "install", "--require-hashes", "-r", ROOT / "scripts/uv-bootstrap.lock")
    uv = tools / "bin/uv"
    if not (DEPS / "venv/bin/python").exists():
        run(uv, "venv", "--python", "3.12", DEPS / "venv")
    run(uv, "pip", "sync", "--python", DEPS / "venv/bin/python",
        "--require-hashes", ROOT / "scripts/build-tools.lock")
    patch = ROOT / lock["gst_analytics_patch"]["path"]
    if not verify(patch, lock["gst_analytics_patch"]["sha256"]):
        raise RuntimeError("GstAnalytics patch differs from lock")
    refreshed = {}
    for archive in lock["gstreamer"]:
        path = DEPS / "src" / Path(archive["url"]).name
        download(archive["url"], path, archive["sha256"])
        refreshed[archive["name"]] = extract_source(path,
            DEPS / "src" / f"{archive['name']}-{archive['version']}", archive["sha256"])
    bad = DEPS / "src/gst-plugins-bad-1.28.2"
    if not (bad / "gst-libs/gst/analytics/gstanalyticskeypointmtd.h").exists():
        run("git", "apply", "-p3", "--exclude=*/GstAnalytics.py", f"--directory={bad.relative_to(ROOT)}", patch)
    # Read the same environment that developers use; bash preserves exact paths.
    raw = subprocess.check_output(["bash", "-c", "source scripts/env.sh && env -0"], cwd=ROOT)
    env = {item.split(b"=", 1)[0].decode(): item.split(b"=", 1)[1].decode()
           for item in raw.split(b"\0") if b"=" in item}
    variants = {
        "gstreamer": ["-Dtests=disabled", "-Dexamples=disabled", "-Dintrospection=disabled", "-Ddoc=disabled", "-Dbenchmarks=disabled", "-Dcheck=disabled", "-Dlibdw=disabled", "-Dlibunwind=disabled", "-Dptp-helper=disabled"],
        "gst-plugins-base": ["-Dauto_features=disabled", "-Dapp=enabled", "-Dvideoconvertscale=enabled", "-Dvideotestsrc=enabled", "-Dtypefind=enabled", "-Dplayback=enabled", "-Dgio=enabled", "-Dtcp=enabled"],
        "gst-plugins-bad": ["-Dauto_features=disabled", "-Dva=enabled", "-Dvideoparsers=enabled"],
        "gst-plugins-good": ["-Dauto_features=disabled", "-Dmultifile=enabled", "-Drtp=enabled", "-Drtpmanager=enabled", "-Drtsp=enabled", "-Dudp=enabled", "-Dmatroska=enabled", "-Disomp4=enabled"],
    }
    variants["gst-rtsp-server"] = ["-Dauto_features=disabled", "-Dtests=disabled", "-Dexamples=disabled", "-Dintrospection=disabled"]
    for name, options in variants.items():
        build = DEPS / f"locked-build-{name}"
        command = ["meson", "setup"]
        if (build / "build.ninja").exists():
            command += ["--wipe" if refreshed[name] else "--reconfigure"]
        run(*command, build, DEPS / "src" / f"{name}-1.28.2", f"--prefix={DEPS}/prefix", "--libdir=lib", *options, env=env)
        run("meson", "compile", "-C", build, "-j", args.jobs, env=env)
        run("meson", "install", "-C", build, env=env)
    extra = ["-DAMDVA_BUILD_MOBILECLIP=OFF", "-DAMDVA_BUILD_NPU=OFF"]
    if args.ryzen_ai_venv:
        extra = ["-DAMDVA_BUILD_MOBILECLIP=ON", f"-DAMDVA_BUILD_NPU={'OFF' if args.mobileclip_backend == 'gpu' else 'ON'}",
                 f"-DAMDVA_ORT_ROOT={args.ryzen_ai_venv.resolve()}/onnxruntime"]
    run("cmake", "-S", ROOT, "-B", ROOT / "build", *extra, f"-DAMDVA_ROCM_ROOT={env['AMDVA_ROCM_ROOT']}",
        f"-DAMDVA_MIGRAPHX_ROOT={env['AMDVA_MIGRAPHX_ROOT']}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_INSTALL_PREFIX={DEPS}/prefix", env=env)
    run("cmake", "--build", ROOT / "build", "-j", args.jobs, env=env)
    run("ctest", "--test-dir", ROOT / "build", "--output-on-failure", env=env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
